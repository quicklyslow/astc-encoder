// SPDX-License-Identifier: Apache-2.0
// ----------------------------------------------------------------------------
// Copyright 2011-2021 Arm Limited
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy
// of the License at:
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations
// under the License.
// ----------------------------------------------------------------------------

/**
 * @brief Functions and data declarations.
 */

#ifndef ASTCENC_INTERNAL_INCLUDED
#define ASTCENC_INTERNAL_INCLUDED

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <type_traits>

#include "astcenc.h"
#include "astcenc_mathlib.h"
#include "astcenc_vecmathlib.h"

/**
 * @brief Make a promise to the compiler's optimizer.
 *
 * A promise is an expression that the optimizer is can assume is true for to help it generate
 * faster code. Common use cases for this are to promise that a for loop will iterate more than
 * once, or that the loop iteration count is a multiple of a vector length, which avoids pre-loop
 * checks and can avoid loop tails if loops are unrolled by the auto-vectorizer.
 */
#if defined(NDEBUG)
	#if !defined(__clang__) && defined(_MSC_VER)
		#define promise(cond) __assume(cond)
	#elif defined(__clang__)
		#if __has_builtin(__builtin_assume)
			#define promise(cond) __builtin_assume(cond)
		#elif __has_builtin(__builtin_unreachable)
			#define promise(cond) if(!(cond)) { __builtin_unreachable(); }
		#else
			#define promise(cond)
		#endif
	#else // Assume GCC
		#define promise(cond) if(!(cond)) { __builtin_unreachable(); }
	#endif
#else
	#define promise(cond) assert(cond)
#endif

/* ============================================================================
  Constants
============================================================================ */
/** @brief The maximum number of components a block can support. */
static constexpr unsigned int BLOCK_MAX_COMPONENTS { 4 };

/** @brief The maximum number of partitions a block can support. */
static constexpr unsigned int BLOCK_MAX_PARTITIONS { 4 };

/** @brief The number of partitionings, per partition count, suported by the ASTC format. */
static constexpr unsigned int BLOCK_MAX_PARTITIONINGS { 1024 };

/** @brief The maximum number of texels a block can support (6x6x6 block). */
static constexpr unsigned int BLOCK_MAX_TEXELS { 216 };

/** @brief The maximum number of weights used during partition selection for texel clustering. */
static constexpr uint8_t BLOCK_MAX_KMEANS_TEXELS { 64 };

/** @brief The maximum number of weights a block can support. */
static constexpr unsigned int BLOCK_MAX_WEIGHTS { 64 };

/** @brief The maximum number of weights a block can support per plane in 2 plane mode. */
static constexpr unsigned int BLOCK_MAX_WEIGHTS_2PLANE { BLOCK_MAX_WEIGHTS / 2 };

/** @brief The minimum number of weight bits a candidate encoding must encode. */
static constexpr unsigned int BLOCK_MIN_WEIGHT_BITS { 24 };

/** @brief The maximum number of weight bits a candidate encoding can encode. */
static constexpr unsigned int BLOCK_MAX_WEIGHT_BITS { 96 };

/** @brief The index indicating a bad (unused) block mode in the remap array. */
static constexpr uint16_t BLOCK_BAD_BLOCK_MODE { 0xFFFFu };

/** @brief The number of partition index bits supported by the ASTC format . */
static constexpr unsigned int PARTITION_INDEX_BITS { 10 };

/** @brief The offset of the plane 2 weights in shared weight arrays. */
static constexpr unsigned int WEIGHTS_PLANE2_OFFSET { BLOCK_MAX_WEIGHTS_2PLANE };

/** @brief The sum of quantized weights for one texel. */
static constexpr float WEIGHTS_TEXEL_SUM { 16.0f };

/** @brief The number of block modes suported by the ASTC format. */
static constexpr unsigned int WEIGHTS_MAX_BLOCK_MODES { 2048 };

/** @brief The number of weight grid decimation modes suported by the ASTC format. */
static constexpr unsigned int WEIGHTS_MAX_DECIMATION_MODES { 87 };

/** @brief The high default error used to initialize error trackers. */
static constexpr float ERROR_CALC_DEFAULT { 1e30f };

/**
 * @brief The minimum texel count for a block to use the one partition fast path.
 *
 * This setting skips 4x4 and 5x4 block sizes.
 */
static constexpr unsigned int TUNE_MIN_TEXELS_MODE0_FASTPATH { 24 };

/**
 * @brief The maximum number of candidate encodings tested for each encoding mode..
 *
 * This can be dynamically reduced by the compression quality preset.
 */
static constexpr unsigned int TUNE_MAX_TRIAL_CANDIDATES { 4 };


static_assert((BLOCK_MAX_TEXELS % ASTCENC_SIMD_WIDTH) == 0,
              "BLOCK_MAX_TEXELS must be multiple of ASTCENC_SIMD_WIDTH");

static_assert((BLOCK_MAX_WEIGHTS % ASTCENC_SIMD_WIDTH) == 0,
              "BLOCK_MAX_WEIGHTS must be multiple of ASTCENC_SIMD_WIDTH");

static_assert((WEIGHTS_MAX_BLOCK_MODES % ASTCENC_SIMD_WIDTH) == 0,
              "WEIGHTS_MAX_BLOCK_MODES must be multiple of ASTCENC_SIMD_WIDTH");


/* ============================================================================
  Parallel execution control
============================================================================ */

/**
 * @brief A simple counter-based manager for parallel task execution.
 *
 * The task processing execution consists of:
 *
 *     * A single-threaded init stage.
 *     * A multi-threaded processing stage.
 *     * A condition variable so threads can wait for processing completion.
 *
 * The init stage will be executed by the first thread to arrive in the critical section, there is
 * no main thread in the thread pool.
 *
 * The processing stage uses dynamic dispatch to assign task tickets to threads on an on-demand
 * basis. Threads may each therefore executed different numbers of tasks, depending on their
 * processing complexity. The task queue and the task tickets are just counters; the caller must map
 * these integers to an actual processing partition in a specific problem domain.
 *
 * The exit wait condition is needed to ensure processing has finished before a worker thread can
 * progress to the next stage of the pipeline. Specifically a worker may exit the processing stage
 * because there are no new tasks to assign to it while other worker threads are still processing.
 * Calling @c wait() will ensure that all other worker have finished before the thread can proceed.
 *
 * The basic usage model:
 *
 *     // --------- From single-threaded code ---------
 *
 *     // Reset the tracker state
 *     manager->reset()
 *
 *     // --------- From multi-threaded code ---------
 *
 *     // Run the stage init; only first thread actually runs the lambda
 *     manager->init(<lambda>)
 *
 *     do
 *     {
 *         // Request a task assignment
 *         uint task_count;
 *         uint base_index = manager->get_tasks(<granule>, task_count);
 *
 *         // Process any tasks we were given (task_count <= granule size)
 *         if (task_count)
 *         {
 *             // Run the user task processing code for N tasks here
 *             ...
 *
 *             // Flag these tasks as complete
 *             manager->complete_tasks(task_count);
 *         }
 *     } while (task_count);
 *
 *     // Wait for all threads to complete tasks before progressing
 *     manager->wait()
 *
  *     // Run the stage term; only first thread actually runs the lambda
 *     manager->term(<lambda>)
 */
class ParallelManager
{
private:
	/** @brief Lock used for critical section and condition synchronization. */
	std::mutex m_lock;

	/** @brief True if the stage init() step has been executed. */
	bool m_init_done;

	/** @brief True if the stage term() step has been executed. */
	bool m_term_done;

	/** @brief Contition variable for tracking stage processing completion. */
	std::condition_variable m_complete;

	/** @brief Number of tasks started, but not necessarily finished. */
	std::atomic<unsigned int> m_start_count;

	/** @brief Number of tasks finished. */
	unsigned int m_done_count;

	/** @brief Number of tasks that need to be processed. */
	unsigned int m_task_count;

public:
	/** @brief Create a new ParallelManager. */
	ParallelManager()
	{
		reset();
	}

	/**
	 * @brief Reset the tracker for a new processing batch.
	 *
	 * This must be called from single-threaded code before starting the multi-threaded procesing
	 * operations.
	 */
	void reset()
	{
		m_init_done = false;
		m_term_done = false;
		m_start_count = 0;
		m_done_count = 0;
		m_task_count = 0;
	}

	/**
	 * @brief Trigger the pipeline stage init step.
	 *
	 * This can be called from multi-threaded code. The first thread to hit this will process the
	 * initialization. Other threads will block and wait for it to complete.
	 *
	 * @param init_func   Callable which executes the stage initialization. It must return the
	 *                    total number of tasks in the stage.
	 */
	void init(std::function<unsigned int(void)> init_func)
	{
		std::lock_guard<std::mutex> lck(m_lock);
		if (!m_init_done)
		{
			m_task_count = init_func();
			m_init_done = true;
		}
	}

	/**
	 * @brief Trigger the pipeline stage init step.
	 *
	 * This can be called from multi-threaded code. The first thread to hit this will process the
	 * initialization. Other threads will block and wait for it to complete.
	 *
	 * @param task_count   Total number of tasks needing processing.
	 */
	void init(unsigned int task_count)
	{
		std::lock_guard<std::mutex> lck(m_lock);
		if (!m_init_done)
		{
			m_task_count = task_count;
			m_init_done = true;
		}
	}

	/**
	 * @brief Request a task assignment.
	 *
	 * Assign up to @c granule tasks to the caller for processing.
	 *
	 * @param      granule   Maximum number of tasks that can be assigned.
	 * @param[out] count     Actual number of tasks assigned, or zero if no tasks were assigned.
	 *
	 * @return Task index of the first assigned task; assigned tasks increment from this.
	 */
	unsigned int get_task_assignment(unsigned int granule, unsigned int& count)
	{
		unsigned int base = m_start_count.fetch_add(granule, std::memory_order_relaxed);
		if (base >= m_task_count)
		{
			count = 0;
			return 0;
		}

		count = astc::min(m_task_count - base, granule);
		return base;
	}

	/**
	 * @brief Complete a task assignment.
	 *
	 * Mark @c count tasks as complete. This will notify all threads blocked on @c wait() if this
	 * completes the processing of the stage.
	 *
	 * @param count   The number of completed tasks.
	 */
	void complete_task_assignment(unsigned int count)
	{
		// Note: m_done_count cannot use an atomic without the mutex; this has a race between the
		// update here and the wait() for other threads
		std::unique_lock<std::mutex> lck(m_lock);
		this->m_done_count += count;
		if (m_done_count == m_task_count)
		{
			lck.unlock();
			m_complete.notify_all();
		}
	}

	/**
	 * @brief Wait for stage processing to complete.
	 */
	void wait()
	{
		std::unique_lock<std::mutex> lck(m_lock);
		m_complete.wait(lck, [this]{ return m_done_count == m_task_count; });
	}

	/**
	 * @brief Trigger the pipeline stage term step.
	 *
	 * This can be called from multi-threaded code. The first thread to hit this will process the
	 * thread termintion. Caller must have called @c wait() prior to calling this function to ensure
	 * that processing is complete.
	 *
	 * @param term_func   Callable which executes the stage termination.
	 */
	void term(std::function<void(void)> term_func)
	{
		std::lock_guard<std::mutex> lck(m_lock);
		if (!m_term_done)
		{
			term_func();
			m_term_done = true;
		}
	}
};

/* ============================================================================
  Commonly used data structures
============================================================================ */

/**
 * @brief The ASTC endpoint formats.
 *
 * Note, the values here are used directly in the encoding in the format so do not rearrange.
 */
enum endpoint_formats
{
	FMT_LUMINANCE = 0,
	FMT_LUMINANCE_DELTA = 1,
	FMT_HDR_LUMINANCE_LARGE_RANGE = 2,
	FMT_HDR_LUMINANCE_SMALL_RANGE = 3,
	FMT_LUMINANCE_ALPHA = 4,
	FMT_LUMINANCE_ALPHA_DELTA = 5,
	FMT_RGB_SCALE = 6,
	FMT_HDR_RGB_SCALE = 7,
	FMT_RGB = 8,
	FMT_RGB_DELTA = 9,
	FMT_RGB_SCALE_ALPHA = 10,
	FMT_HDR_RGB = 11,
	FMT_RGBA = 12,
	FMT_RGBA_DELTA = 13,
	FMT_HDR_RGB_LDR_ALPHA = 14,
	FMT_HDR_RGBA = 15
};

/**
 * @brief The ASTC quantization methods.
 *
 * Note, the values here are used directly in the encoding in the format so do not rearrange.
 */
enum quant_method
{
	QUANT_2 = 0,
	QUANT_3 = 1,
	QUANT_4 = 2,
	QUANT_5 = 3,
	QUANT_6 = 4,
	QUANT_8 = 5,
	QUANT_10 = 6,
	QUANT_12 = 7,
	QUANT_16 = 8,
	QUANT_20 = 9,
	QUANT_24 = 10,
	QUANT_32 = 11,
	QUANT_40 = 12,
	QUANT_48 = 13,
	QUANT_64 = 14,
	QUANT_80 = 15,
	QUANT_96 = 16,
	QUANT_128 = 17,
	QUANT_160 = 18,
	QUANT_192 = 19,
	QUANT_256 = 20
};

/**
 * @brief The number of levels use by an ASTC quantization method.
 *
 * @param method   The quantization method
 *
 * @return   The number of levels used by @c method.
 */
static inline unsigned int get_quant_level(quant_method method)
{
	switch(method)
	{
	case QUANT_2:   return   2;
	case QUANT_3:   return   3;
	case QUANT_4:   return   4;
	case QUANT_5:   return   5;
	case QUANT_6:   return   6;
	case QUANT_8:   return   8;
	case QUANT_10:  return  10;
	case QUANT_12:  return  12;
	case QUANT_16:  return  16;
	case QUANT_20:  return  20;
	case QUANT_24:  return  24;
	case QUANT_32:  return  32;
	case QUANT_40:  return  40;
	case QUANT_48:  return  48;
	case QUANT_64:  return  64;
	case QUANT_80:  return  80;
	case QUANT_96:  return  96;
	case QUANT_128: return 128;
	case QUANT_160: return 160;
	case QUANT_192: return 192;
	case QUANT_256: return 256;
	}

	// Unreachable - the enum is fully described
	return 0;
}

/**
 * @brief Computed metrics about a partition in a block.
 */
struct partition_metrics
{
	/** @brief The sum of the error weights for texels in this partition. */
	vfloat4 error_weight;

	/** @brief The color scale factor used to weight color channels. */
	vfloat4 color_scale;

	/** @brief The 1 / color_scale used to avoid divisions. */
	vfloat4 icolor_scale;

	/** @brief The error-weighted average color in the partition. */
	vfloat4 avg;

	/** @brief The dominant error-weighted direction in the partition. */
	vfloat4 dir;
};

/**
 * @brief Computed lines for a a three component analysis.
 */
struct partition_lines3
{
	/** @brief Line for uncorrelated chroma. */
	line3 uncor_line;

	/** @brief Line for correlated chroma, passing though the origin. */
	line3 samec_line;

	/** @brief Postprocessed line for uncorrelated chroma. */
	processed_line3 uncor_pline;

	/** @brief Postprocessed line for correlated chroma, passing though the origin. */
	processed_line3 samec_pline;

	/** @brief The length of the line for uncorrelated chroma. */
	float uncor_line_len;

	/** @brief The length of the line for correlated chroma. */
	float samec_line_len;
};

/**
 * @brief The partition information for a single partition.
 *
 * ASTC has a total of 1024 candidate partitions for each of 2/3/4 partition counts, although this
 * 1024 includes seeds that generate duplicates of other seeds and seeds that generate completely
 * empty partitions. These are both valid encodings, but astcenc will skip both during compression
 * as they are not useful.
 */
struct partition_info
{
	/** @brief The number of partitions in this partitioning. */
	unsigned int partition_count;

	/**
	 * @brief The number of texels in each partition.
	 *
	 * Note that some seeds result in zero texels assigned to a partition are valid, but are skipped
	 * by this compressor as there is no point spending bits encoding an unused color endpoint.
	 */
	uint8_t partition_texel_count[BLOCK_MAX_PARTITIONS];

	/** @brief The partition of each texel in the block. */
	uint8_t partition_of_texel[BLOCK_MAX_TEXELS];

	/** @brief The list of texels in each partition. */
	uint8_t texels_of_partition[BLOCK_MAX_PARTITIONS][BLOCK_MAX_TEXELS];

	/** @brief The canonical partition coverage pattern used during block partition search. */
	uint64_t coverage_bitmaps[BLOCK_MAX_PARTITIONS];
};

/**
 * @brief The weight grid information for a single decimation pattern.
 *
 * ASTC can store one weight per texel, but is also capable of storing lower resoution weight grids
 * that are interpolated during decompression to assign a with to a texel. Storing fewer weights
 * can free up a substantial amount of bits that we can then spend on more useful things, such as
 * more accurate endpoints and weights, or additional partitions.
 *
 * This data structure is used to store information about a single weight grid decimation pattern,
 * for a single block size.
*/
struct decimation_info
{
	/** @brief The total number of texels in the block. */
	uint8_t texel_count;

	/** @brief The total number of weights stored. */
	uint8_t weight_count;

	/** @brief The number of stored weights in the X dimension. */
	uint8_t weight_x;

	/** @brief The number of stored weights in the Y dimension. */
	uint8_t weight_y;

	/** @brief The number of stored weights in the Z dimension. */
	uint8_t weight_z;

	/** @brief The number of stored weights that contribute to each texel, between 1 and 4. */
	uint8_t texel_weight_count[BLOCK_MAX_TEXELS];

	/** @brief The weight index of the N weights that need to be interpolated for each texel. */
	uint8_t texel_weights_4t[4][BLOCK_MAX_TEXELS];

	/** @brief The bilinear interpolation weighting of the N input weights for each texel, between 0 and 16. */
	uint8_t texel_weights_int_4t[4][BLOCK_MAX_TEXELS];

	/** @brief The bilinear interpolation weighting of the N input weights for each texel, between 0 and 1. */
	alignas(ASTCENC_VECALIGN) float texel_weights_float_4t[4][BLOCK_MAX_TEXELS];

	/** @brief The number of texels that each stored weight contributes to. */
	uint8_t weight_texel_count[BLOCK_MAX_WEIGHTS];

	/** @brief The list of weights that contribute to each texel. */
	uint8_t weight_texel[BLOCK_MAX_TEXELS][BLOCK_MAX_WEIGHTS];

	/** @brief The list of weight indices that contribute to each texel. */
	alignas(ASTCENC_VECALIGN) float weights_flt[BLOCK_MAX_TEXELS][BLOCK_MAX_WEIGHTS];

	/**
	 * @brief Folded structure for faster access:
	 *     texel_weights_texel[i][j][.] = texel_weights[.][weight_texel[i][j]]
	 */
	uint8_t texel_weights_texel[BLOCK_MAX_WEIGHTS][BLOCK_MAX_TEXELS][4];

	/**
	 * @brief Folded structure for faster access:
	 *     texel_weights_float_texel[i][j][.] = texel_weights_float[.][weight_texel[i][j]]
	 */
	float texel_weights_float_texel[BLOCK_MAX_WEIGHTS][BLOCK_MAX_TEXELS][4];
};

/**
 * @brief Metadata for single block mode for a specific block size.
 */
struct block_mode
{
	/** @brief The block mode index in the ASTC encoded form. */
	uint16_t mode_index;

	/** @brief The decimation mode index in the compressor reindexed list. */
	uint8_t decimation_mode;

	/** @brief The weight quantization used by this block mode. */
	uint8_t quant_mode;

	/** @brief Is a dual weight plane used by this block mode? */
	uint8_t is_dual_plane : 1;

	/** @brief Is this mode enabled in the current search preset? */
	uint8_t percentile_hit : 1;

	/**
	 * @brief Get the weight quantization used by this block mode.
	 *
	 * @return The quantization level.
	 */
	inline quant_method get_weight_quant_mode() const
	{
		return (quant_method)this->quant_mode;
	}
};

/**
 * @brief Metadata for single decimation mode for a specific block size.
 */
struct decimation_mode
{
	/** @brief The max weight precision for 1 plane, or -1 if not supported. */
	int8_t maxprec_1plane;

	/** @brief The max weight precision for 2 planes, or -1 if not supported. */
	int8_t maxprec_2planes;

	/** @brief Is this mode enabled in the current search preset? */
	uint8_t percentile_hit;
};

/**
 * @brief Data tables for a single block size.
 *
 * The decimation tables store the information to apply weight grid dimension reductions. We only
 * store the decimation modes that are actually needed by the current context; many of the possible
 * modes will be unused (too many weights for the current block size or disabled by heuristics). The
 * actual number of weights stored is @c decimation_mode_count, and the @c decimation_modes and
 * @c decimation_tables arrays store the active modes contiguously at the start of the array. These
 * entries are not stored in any particuar order.
 *
 * The block mode tables store the unpacked block mode settings. Block modes are stored in the
 * compressed block as an 11 bit field, but for any given block size and set of compressor
 * heuristics, only a subset of the block modes will be used. The actual number of block modes
 * stored is indicated in @c block_mode_count, and the @c block_modes array store the active modes
 * contiguously at the start of the array. These entries are stored in incrementing "packed" value
 * order, which doesn't mean much once unpacked. To allow decompressors to reference the packed data
 * efficiently the @c block_mode_packed_index array stores the mapping between physical ID and the
 * actual remapped array index.
 */
struct block_size_descriptor
{
	/** @brief The block X dimension, in texels. */
	uint8_t xdim;

	/** @brief The block Y dimension, in texels. */
	uint8_t ydim;

	/** @brief The block Z dimension, in texels. */
	uint8_t zdim;

	/** @brief The block total texel count. */
	uint8_t texel_count;

	/** @brief The number of stored decimation modes. */
	unsigned int decimation_mode_count;

	/**
	 * @brief The number of stored decimation modes which are "always" modes.
	 *
	 * Always modes are stored at the start of the decimation_modes list.
	 */
	unsigned int always_decimation_mode_count;

	/** @brief The number of stored block modes. */
	unsigned int block_mode_count;

	/**
	 * @brief The number of stored block modes which are "always" modes.
	 *
	 * Always modes are stored at the start of the block_modes list.
	 */
	unsigned int always_block_mode_count;

	/** @brief The active decimation modes, stored in low indices. */
	decimation_mode decimation_modes[WEIGHTS_MAX_DECIMATION_MODES];

	/** @brief The active decimation tables, stored in low indices. */
	const decimation_info *decimation_tables[WEIGHTS_MAX_DECIMATION_MODES];

	/** @brief The packed block mode array index, or @c BLOCK_BAD_BLOCK_MODE if not active. */
	uint16_t block_mode_packed_index[WEIGHTS_MAX_BLOCK_MODES];

	/** @brief The active block modes, stored in low indices. */
	block_mode block_modes[WEIGHTS_MAX_BLOCK_MODES];

	/** @brief The partition tables for all of the possible partitions. */
	partition_info partitions[(3 * BLOCK_MAX_PARTITIONINGS) + 1];

	/** @brief The active texels for k-means partition selection. */
	uint8_t kmeans_texels[BLOCK_MAX_KMEANS_TEXELS];

	/**
	 * @brief Get the block mode structure for index @c block_mode.
	 *
	 * This function can only return block modes that are enabled by the current compressor config.
	 * Decompression from an arbitrary source should not use this without first checking that the
	 * packed block mode index is not @c BLOCK_BAD_BLOCK_MODE.
	 *
	 * @param block_mode   The packed block mode index.
	 *
	 * @return The block mode structure.
	 */
	const block_mode& get_block_mode(unsigned int block_mode) const
	{
		unsigned int packed_index = this->block_mode_packed_index[block_mode];
		assert(packed_index != BLOCK_BAD_BLOCK_MODE && packed_index < this->block_mode_count);
		return block_modes[packed_index];
	}

	/**
	 * @brief Get the decimation mode structure for index @c decimation_mode.
	 *
	 * This function can only return decimation modes that are enabled by the current compressor
	 * config. The mode array is stored packed, but this is only ever indexed by the packed index
	 * stored in the @c block_mode and never exists in an unpacked form.
	 *
	 * @param decimation_mode   The packed decimation mode index.
	 *
	 * @return The decimation mode structure.
	 */
	const decimation_mode& get_decimation_mode(unsigned int decimation_mode) const
	{
		return this->decimation_modes[decimation_mode];
	}

	/**
	 * @brief Get the decimation info structure for index @c decimation_mode.
	 *
	 * This function can only return decimation modes that are enabled by the current compressor
	 * config. The mode array is stored packed, but this is only ever indexed by the packed index
	 * stored in the @c block_mode and never exists in an unpacked form.
	 *
	 * @param decimation_mode   The packed decimation mode index.
	 *
	 * @return The decimation info structure.
	 */
	const decimation_info& get_decimation_info(unsigned int decimation_mode) const
	{
		return *this->decimation_tables[decimation_mode];
	}

	/**
	 * @brief Get the partition info table for a given partition count.
	 *
	 * @param partition_count   The number of partitions we want the table for.
	 *
	 * @return The pointer to the table of 1024 entries (for 2/3/4 parts) or 1 entry (for 1 part).
	 */
	const partition_info* get_partition_table(unsigned int partition_count) const
	{
		if (partition_count == 1)
		{
			partition_count = 5;
		}
		unsigned int index = (partition_count - 2) * BLOCK_MAX_PARTITIONINGS;
		return this->partitions + index;
	}

	/**
	 * @brief Get the partition info structure for a given partition count and seed.
	 *
	 * @param partition_count   The number of partitions we want the info for.
	 * @param index             The partition seed (between 0 and 1023).
	 *
	 * @return The partition info structure.
	 */
	const partition_info& get_partition_info(unsigned int partition_count, unsigned int index) const
	{
		return  get_partition_table(partition_count)[index];
	}
};

/**
 * @brief The image data for a single block.
 *
 * The @c data_[rgba] fields store the image data in an encoded SoA float form designed for easy
 * vectorization. Input data is converted to float and stored as values between 0 and 65535. LDR
 * data is stored as direct UNORM data, HDR data is stored as LNS data.
 *
 * The @c rgb_lns and @c alpha_lns fields that assigned a per-texel use of HDR are only used during
 * decompression. The current compressor will always use HDR endpoint formats when in HDR mode.
 */
struct image_block
{
	/** @brief The input (compress) or output (decompress) data for the red color component. */
	float data_r[BLOCK_MAX_TEXELS];

	/** @brief The input (compress) or output (decompress) data for the green color component. */
	float data_g[BLOCK_MAX_TEXELS];

	/** @brief The input (compress) or output (decompress) data for the blue color component. */
	float data_b[BLOCK_MAX_TEXELS];

	/** @brief The input (compress) or output (decompress) data for the alpha color component. */
	float data_a[BLOCK_MAX_TEXELS];

	/** @brief The original data for texel 0 for constant color block encoding. */
	vfloat4 origin_texel;

	/** @brief The min component value of all texels in the block. */
	vfloat4 data_min;

	/** @brief The max component value of all texels in the block. */
	vfloat4 data_max;

	/** @brief Is this greyscale block where R == G == B for all texels? */
	bool grayscale;

	/** @brief Set to 1 if a texel is using HDR RGB endpoints (decompression only). */
	uint8_t rgb_lns[BLOCK_MAX_TEXELS];

	/** @brief Set to 1 if a texel is using HDR alpha endpoints (decompression only). */
	uint8_t alpha_lns[BLOCK_MAX_TEXELS];

	/** @brief The X position of this block in the input or output image. */
	unsigned int xpos;

	/** @brief The Y position of this block in the input or output image. */
	unsigned int ypos;

	/** @brief The Z position of this block in the input or output image. */
	unsigned int zpos;

	/**
	 * @brief Get an RGBA texel value from the data.
	 *
	 * @param index   The texel index.
	 *
	 * @return The texel in RGBA component ordering.
	 */
	inline vfloat4 texel(unsigned int index) const
	{
		return vfloat4(data_r[index],
		               data_g[index],
		               data_b[index],
		               data_a[index]);
	}

	/**
	 * @brief Get an RGB texel value from the data.
	 *
	 * @param index   The texel index.
	 *
	 * @return The texel in RGB0 component ordering.
	 */
	inline vfloat4 texel3(unsigned int index) const
	{
		return vfloat3(data_r[index],
		               data_g[index],
		               data_b[index]);
	}

	/**
	 * @brief Get the default alpha value for endpoints that don't store it.
	 *
	 * The default depends on whether the alpha endpoint is LDR or HDR.
	 *
	 * @return The alpha value in the scaled range used by the compressor.
	 */
	inline float get_default_alpha() const
	{
		return this->alpha_lns[0] ? (float)0x7800 : (float)0xFFFF;
	}

	/**
	 * @brief Test if a single color channel is constant across the block.
	 *
	 * Constant color channels are easier to compress as interpolating between two identical colors
	 * always returns the same value, irrespective of the weight used. They therefore can be ignored
	 * for the purposes of weight selection and use of a second weight plane.
	 *
	 * @return @c true if the channel is constant across the block, @c false otherwise.
	 */
	inline bool is_constant_channel(int channel) const
	{
		vmask4 lane_mask = vint4::lane_id() == vint4(channel);
		vmask4 color_mask = this->data_min == this->data_max;
		return any(lane_mask & color_mask);
	}

	/**
	 * @brief Test if this block is a luminance block with constant 1.0 alpha.
	 *
	 * @return @c true if the block is a luminance block , @c false otherwise.
	 */
	inline bool is_luminance() const
	{
		float default_alpha = this->get_default_alpha();
		bool alpha1 = (this->data_min.lane<3>() == default_alpha) &&
		              (this->data_max.lane<3>() == default_alpha);
		return this->grayscale && alpha1;
	}

	/**
	 * @brief Test if this block is a luminance block with variable alpha.
	 *
	 * @return @c true if the block is a luminance + alpha block , @c false otherwise.
	 */
	inline bool is_luminancealpha() const
	{
		float default_alpha = this->get_default_alpha();
		bool alpha1 = (this->data_min.lane<3>() == default_alpha) &&
		              (this->data_max.lane<3>() == default_alpha);
		return this->grayscale && !alpha1;
	}
};

/**
 * @brief Data structure representing per-texel and per-component error weights for a block.
 *
 * This structure stores a multiplier for the error weight to apply to each component when computing
 * block errors. This can be used as a general purpose technique to to amplify or diminish the
 * significance of texels and individual color components, based on what is being stored and the
 * compressor heuristics. It can be applied in many different ways, some of which are outlined in
 * the description below (this is not exhaustive).
 *
 * For blocks that span the edge of the texture, the weighting for texels outside of the texture
 * bounds can zeroed to maximize the quality of the texels inside the texture.
 *
 * For textures storing fewer than 4 components the weighting for color components that are unused
 * can be zeroed to maximize the quality of the components that are used. This is particularly
 * important for two component textures, which must be imported in LLLA format to match the two
 * component endpoint encoding. Without manual component weighting to correct significance the "L"
 * would be treated as three times more important than A because of the replication.
 *
 * For HDR textures we can use perceptual weighting which os approximately inverse to the luminance
 * of a texel.
 *
 * For normal maps we can use perceptual weighting which assigns higher weight to low-variability
 * regions than to high-variability regions, ensuring smooth surfaces don't pick up artifacts.
 *
 * For transparent texels we can multiply the RGB weights by the alpha value, ensuring that
 * the least transprent texels maintain the highest accuracy.
 */
struct error_weight_block
{
	/** @brief Block error weighted RGBA sum for whole block / 1 partition. */
	vfloat4 block_error_weighted_rgba_sum;

	/** @brief Block error sum for whole block / 1 partition. */
	vfloat4 block_error_weight_sum;

	/** @brief The full per texel per component error weights. */
	vfloat4 error_weights[BLOCK_MAX_TEXELS];


	/** @brief The full per texel per component error weights. */
	float texel_weight[BLOCK_MAX_TEXELS];


	/** @brief The average of the GBA error weights per texel. */
	float texel_weight_gba[BLOCK_MAX_TEXELS];

	/** @brief The average of the RBA error weights per texel. */
	float texel_weight_rba[BLOCK_MAX_TEXELS];

	/** @brief The average of the RGA error weights per texel. */
	float texel_weight_rga[BLOCK_MAX_TEXELS];

	/** @brief The average of the RGB error weights per texel. */
	float texel_weight_rgb[BLOCK_MAX_TEXELS];


	/** @brief The average of the RG error weights per texel. */
	float texel_weight_rg[BLOCK_MAX_TEXELS];

	/** @brief The average of the RB error weights per texel. */
	float texel_weight_rb[BLOCK_MAX_TEXELS];

	/** @brief The average of the GB error weights per texel. */
	float texel_weight_gb[BLOCK_MAX_TEXELS];


	/** @brief The individual R component error weights per texel. */
	float texel_weight_r[BLOCK_MAX_TEXELS];

	/** @brief The individual G component error weights per texel. */
	float texel_weight_g[BLOCK_MAX_TEXELS];

	/** @brief The individual B component error weights per texel. */
	float texel_weight_b[BLOCK_MAX_TEXELS];

	/** @brief The individual A component error weights per texel. */
	float texel_weight_a[BLOCK_MAX_TEXELS];
};

/**
 * @brief Data structure storing the color endpoints for a block.
 */
struct endpoints
{
	/** @brief The number of partition endpoints stored. */
	unsigned int partition_count;

	/** @brief The colors for endpoint 0. */
	vfloat4 endpt0[BLOCK_MAX_PARTITIONS];

	/** @brief The colors for endpoint 1. */
	vfloat4 endpt1[BLOCK_MAX_PARTITIONS];
};

/**
 * @brief Data structure storing the color endpoints and weights.
 */
struct endpoints_and_weights
{
	/** @brief True if all active values in weight_error_scale are the same. */
	bool is_constant_weight_error_scale;

	/** @brief The color endpoints. */
	endpoints ep;

	/** @brief The ideal weight for each texel; may be undecimated or decimated. */
	alignas(ASTCENC_VECALIGN) float weights[BLOCK_MAX_TEXELS];

	/** @brief The ideal weight error scaling for each texel; may be undecimated or decimated. */
	alignas(ASTCENC_VECALIGN) float weight_error_scale[BLOCK_MAX_TEXELS];
};

/**
 * @brief Utility storing estimated errors from choosing particular endpoint encodings.
 */
struct encoding_choice_errors
{
	/** @brief Error of using LDR RGB-scale instead of complete endpoints. */
	float rgb_scale_error;

	/** @brief Error of using HDR RGB-scale instead of complete endpoints. */
	float rgb_luma_error;

	/** @brief Error of using luminance instead of RGB. */
	float luminance_error;

	/** @brief Error of discarding alpha and using a constant 1.0 alpha. */
	float alpha_drop_error;

	/** @brief Can we use delta offset encoding? */
	bool can_offset_encode;

	/** @brief CAn we use blue contraction encoding? */
	bool can_blue_contract;
};

/**
 * @brief Preallocated working buffers, allocated per thread during context creation.
 */
struct alignas(ASTCENC_VECALIGN) compression_working_buffers
{
	/** @brief Ideal endpoints and weights for plane 1. */
	endpoints_and_weights ei1;

	/** @brief Ideal endpoints and weights for plane 2. */
	endpoints_and_weights ei2;

	/** @brief Ideal decimated endpoints and weights for plane 1. */
	endpoints_and_weights eix1[WEIGHTS_MAX_DECIMATION_MODES];

	/** @brief Ideal decimated endpoints and weights for plane 2. */
	endpoints_and_weights eix2[WEIGHTS_MAX_DECIMATION_MODES];

	/** @brief The error weight block for the current thread. */
	error_weight_block ewb;

	/**
	 * @brief Decimated ideal weight values.
	 *
	 * For two plane encodings, second plane weights start at @c WEIGHTS_PLANE2_OFFSET offsets.
	 */
	alignas(ASTCENC_VECALIGN) float dec_weights_ideal_value[WEIGHTS_MAX_DECIMATION_MODES * BLOCK_MAX_WEIGHTS];

	/**
	 * @brief Decimated ideal weight significance.
	 *
	 * For two plane encodings, second plane weights start at @c WEIGHTS_PLANE2_OFFSET offsets.
	 */
	alignas(ASTCENC_VECALIGN) float dec_weights_ideal_sig[WEIGHTS_MAX_DECIMATION_MODES * BLOCK_MAX_WEIGHTS];

	/**
	 * @brief Decimated and quantized weight values stored in the unpacked quantized weight range.
	 *
	 * For two plane encodings, second plane weights start at @c WEIGHTS_PLANE2_OFFSET offsets.
	 */
	alignas(ASTCENC_VECALIGN) float dec_weights_quant_uvalue[WEIGHTS_MAX_BLOCK_MODES * BLOCK_MAX_WEIGHTS];

	/**
	 * @brief Decimated and quantized weight values stored in the packed quantized weight range.
	 *
	 * For two plane encodings, second plane weights start at @c WEIGHTS_PLANE2_OFFSET offsets.
	 */
	alignas(ASTCENC_VECALIGN) uint8_t dec_weights_quant_pvalue[WEIGHTS_MAX_BLOCK_MODES * BLOCK_MAX_WEIGHTS];
};

/**
 * @brief Weight quantization transfer table.
 *
 * ASTC can store texel weights at many quantization levels, so for performance we store essential
 * information about each level as a precomputed data structure. Unquantized weights are integers
 * or floats in the range [0, 64].
 *
 * This structure provides a table, used to estimate the closest quantized weight for a given
 * floating-point weight. For each quantized weight, the corresponding unquantized values. For each
 * quantized weight, a previous-value and a next-value.
*/
struct quantization_and_transfer_table
{
	/** @brief The quantization level used */
	quant_method method;

	/** @brief The unscrambled unquantized value. */
	float unquantized_value_unsc[33];

	/** @brief The scrambling order: value[map[i]] == value_unsc[i] */
	int32_t scramble_map[32];

	/** @brief The scrambled unquantized values. */
	uint8_t unquantized_value[32];

	/**
	 * @brief A table of previous-and-next weights, indexed by the current unquantized value.
	 *  * bits 7:0 = previous-index, unquantized
	 *  * bits 15:8 = next-index, unquantized
	 *  * bits 23:16 = previous-index, quantized
	 *  * bits 31:24 = next-index, quantized
	 */
	uint32_t prev_next_values[65];
};


/** @brief The precomputed quant and transfer table. */
extern const quantization_and_transfer_table quant_and_xfer_tables[12];

/** @brief The block is an error block, and will return error color or NaN. */
static constexpr uint8_t SYM_BTYPE_ERROR { 0 };

/** @brief The block is a constant color block using FP16 colors. */
static constexpr uint8_t SYM_BTYPE_CONST_F16 { 1 };

/** @brief The block is a constant color block using UNORM16 colors. */
static constexpr uint8_t SYM_BTYPE_CONST_U16 { 2 };

/** @brief The block is a normal non-constant color block. */
static constexpr uint8_t SYM_BTYPE_NONCONST { 3 };

/**
 * @brief A symbolic representation of a compressed block.
 *
 * The symbolic representation stores the unpacked content of a single
 * @c physical_compressed_block, in a form which is much easier to access for
 * the rest of the compressor code.
 */
struct symbolic_compressed_block
{
	/** @brief The block type, one of the @c SYM_BTYPE_* constants. */
	uint8_t block_type;

	/** @brief The number of partitions; valid for @c NONCONST blocks. */
	uint8_t partition_count;

	/** @brief Non-zero if the color formats matched; valid for @c NONCONST blocks. */
	uint8_t color_formats_matched;

	/** @brief The plane 2 color component, or -1 if single plane; valid for @c NONCONST blocks. */
	// Try unsigned sentintel to avoid signext on load
	int8_t plane2_component;

	/** @brief The block mode; valid for @c NONCONST blocks. */
	uint16_t block_mode;

	/** @brief The partition index; valid for @c NONCONST blocks if 2 or more partitions. */
	uint16_t partition_index;

	/** @brief The endpoint color formats for each partition; valid for @c NONCONST blocks. */
	uint8_t color_formats[BLOCK_MAX_PARTITIONS];

	/** @brief The endpoint color quant mode; valid for @c NONCONST blocks. */
	quant_method quant_mode;

	/** @brief The error of the current encoding; valid for @c NONCONST blocks. */
	float errorval;

	// We can't have both of these at the same time
	union {
		/** @brief The constant color; valid for @c CONST blocks. */
		int constant_color[BLOCK_MAX_COMPONENTS];

		/** @brief The quantized endpoint color pairs; valid for @c NONCONST blocks. */
		uint8_t color_values[BLOCK_MAX_PARTITIONS][8];
	};

	/** @brief The quantized and decimated weights.
	 *
	 * If dual plane, the second plane starts at @c weights[WEIGHTS_PLANE2_OFFSET].
	 */
	uint8_t weights[BLOCK_MAX_WEIGHTS];

	/**
	 * @brief Get the weight quantization used by this block mode.
	 *
	 * @return The quantization level.
	 */
	inline quant_method get_color_quant_mode() const
	{
		return this->quant_mode;
	}
};

/**
 * @brief A physical representation of a compressed block.
 *
 * The physical representation stores the raw bytes of the format in memory.
 */
struct physical_compressed_block
{
	/** @brief The ASTC encoded data for a single block. */
	uint8_t data[16];
};


/**
 * @brief Parameter structure for @c compute_pixel_region_variance().
 *
 * This function takes a structure to avoid spilling arguments to the stack on every function
 * invocation, as there are a lot of parameters.
 */
struct pixel_region_variance_args
{
	/** @brief The image to analyze. */
	const astcenc_image* img;

	/** @brief The RGB component power adjustment. */
	float rgb_power;

	/** @brief The alpha component power adjustment. */
	float alpha_power;

	/** @brief The component swizzle pattern. */
	astcenc_swizzle swz;

	/** @brief Should the algorithm bother with Z axis processing? */
	bool have_z;

	/** @brief The kernel radius for average and variance. */
	unsigned int avg_var_kernel_radius;

	/** @brief The kernel radius for alpha processing. */
	unsigned int alpha_kernel_radius;

	/** @brief The X dimension of the working data to process. */
	unsigned int size_x;

	/** @brief The Y dimension of the working data to process. */
	unsigned int size_y;

	/** @brief The Z dimension of the working data to process. */
	unsigned int size_z;

	/** @brief The X position of first src and dst data in the data set. */
	unsigned int offset_x;

	/** @brief The Y position of first src and dst data in the data set. */
	unsigned int offset_y;

	/** @brief The Z position of first src and dst data in the data set. */
	unsigned int offset_z;

	/** @brief The working memory buffer. */
	vfloat4 *work_memory;
};

/**
 * @brief Parameter structure for @c compute_averages_and_variances_proc().
 */
struct avg_var_args
{
	/** @brief The arguments for the nested variance computation. */
	pixel_region_variance_args arg;

	// The above has a reference to the image altread?
	/** @brief The image X dimensions. */
	unsigned int img_size_x;

	/** @brief The image Y dimensions. */
	unsigned int img_size_y;

	/** @brief The image Z dimensions. */
	unsigned int img_size_z;

	/** @brief The maximum working block dimensions in X and Y dimensions. */
	unsigned int blk_size_xy;

	/** @brief The maximum working block dimensions in Z dimensions. */
	unsigned int blk_size_z;

	/** @brief The working block memory size. */
	unsigned int work_memory_size;
};

#if defined(ASTCENC_DIAGNOSTICS)
/* See astcenc_diagnostic_trace header for details. */
class TraceLog;
#endif

/**
 * @brief The astcenc compression context.
 */
struct astcenc_context
{
	/** @brief The configuration this context was created with. */
	astcenc_config config;

	/** @brief The thread count supported by this context. */
	unsigned int thread_count;

	/** @brief The block size descriptor this context was created with. */
	block_size_descriptor* bsd;

	/*
	 * Fields below here are not needed in a decompress-only build, but some remain as they are
	 * small and it avoids littering the code with #ifdefs. The most significant contributors to
	 * large structure size are omitted.
	 */

	/** @brief The input images averages table, may be @c nullptr if not needed. */
	vfloat4 *input_averages;

	/** @brief The input image RGBA channel variances table, may be @c nullptr if not needed. */
	vfloat4 *input_variances;

	/** @brief The input image alpha channel variances table, may be @c nullptr if not needed. */
	float *input_alpha_averages;


	/** @brief The scratch workign buffers, one per thread (see @c thread_count). */
	compression_working_buffers* working_buffers;

#if !defined(ASTCENC_DECOMPRESS_ONLY)
	/** @brief The pixel region and variance worker arguments. */
	avg_var_args avg_var_preprocess_args;

	/** @brief The per-texel deblocking weights for the current block size. */
	float deblock_weights[BLOCK_MAX_TEXELS];

	/** @brief The parallel manager for averages and variances computation. */
	ParallelManager manage_avg_var;

	/** @brief The parallel manager for compression. */
	ParallelManager manage_compress;
#endif

	/** @brief The parallel manager for decompression. */
	ParallelManager manage_decompress;

#if defined(ASTCENC_DIAGNOSTICS)
	/**
	 * @brief The diagnostic trace logger.
	 *
	 * Note that this is a singleton, so can only be used in single threaded mode. It only exists
	 * here so we have a reference to close the file at the end of the capture.
	 */
	TraceLog* trace_log;
#endif
};

/* ============================================================================
  Functionality for managing block sizes and partition tables.
============================================================================ */

/**
 * @brief Populate the block size descriptor for the target block size.
 *
 * This will also initialize the partition table metadata, which is stored as part of the BSD
 * structure. All initialized block size descriptors must be terminated using a call to
 * @c term_block_size_descriptor() to free resources.
 *
 * @param      x_texels         The number of texels in the block X dimension.
 * @param      y_texels         The number of texels in the block Y dimension.
 * @param      z_texels         The number of texels in the block Z dimension.
 * @param      can_omit_modes   Can we discard modes that astcenc won't use, even if legal?
 * @param      mode_cutoff      The block mode percentile cutoff [0-1].
 * @param[out] bsd              The descriptor to initialize.
 */
void init_block_size_descriptor(
	unsigned int x_texels,
	unsigned int y_texels,
	unsigned int z_texels,
	bool can_omit_modes,
	float mode_cutoff,
	block_size_descriptor& bsd);

/**
 * @brief Terminate a block size descriptor and free associated resources.
 *
 * @param bsd   The descriptor to terminate.
 */
void term_block_size_descriptor(
	block_size_descriptor& bsd);

/**
 * @brief Populate the partition tables for the target block size.
 *
 * Note the @c bsd descriptor must be initialized by calling @c init_block_size_descriptor() before
 * calling this function.
 *
 * @param[out] bsd   The block size information structure to populate.
 */
void init_partition_tables(
	block_size_descriptor& bsd);

/**
 * @brief Get the percentile table for 2D block modes.
 *
 * This is an empirically determined prioritization of which block modes to use in the search in
 * terms of their centile (lower centiles = more useful).
 *
 * Returns a dynamically allocated array; caller must free with delete[].
 *
 * @param xdim The block x size.
 * @param ydim The block y size.
 *
 * @return The unpacked table.
 */
const float *get_2d_percentile_table(
	unsigned int xdim,
	unsigned int ydim);

/**
 * @brief Query if a 2D block size is legal.
 *
 * @return True if legal, false otherwise.
 */
bool is_legal_2d_block_size(
	unsigned int xdim,
	unsigned int ydim);

/**
 * @brief Query if a 3D block size is legal.
 *
 * @return True if legal, false otherwise.
 */
bool is_legal_3d_block_size(
	unsigned int xdim,
	unsigned int ydim,
	unsigned int zdim);

/* ============================================================================
  Functionality for managing BISE quantization and unquantization.
============================================================================ */

/**
 * @brief The precomputed table for quantizing color values.
 *
 * Indexed by [quant_mode][data_value].
 */
extern const uint8_t color_quant_tables[21][256];

/**
 * @brief The precomputed table for unquantizing color values.
 *
 * Indexed by [quant_mode][data_value].
 */
extern const uint8_t color_unquant_tables[21][256];

/**
 * @brief The precomputed quant mode storage table.
 *
 * Indexing by [integercount/2][bits] gives us the quantization level for a given integer count and
 * number of compressed storage bits. Returns -1 for cases where the requested integer count cannot
 * ever fit in the supplied storage size.
 */
extern const int8_t quant_mode_table[17][128];

/**
 * @brief Encode a packed string using BISE.
 *
 * Note that BISE can return strings that are not a whole number of bytes in length, and ASTC can
 * start storing strings in a block at arbitrary bit offsets in the encoded data.
 *
 * @param         quant_level      The BISE alphabet size.
 * @param         character_count  The number of characters in the string.
 * @param         input_data       The unpacked string, one byte per character.
 * @param[in,out] output_data      The output packed string.
 * @param         bit_offset       The starting offset in the output storage.
 */
void encode_ise(
	quant_method quant_level,
	unsigned int character_count,
	const uint8_t* input_data,
	uint8_t* output_data,
	unsigned int bit_offset);

/**
 * @brief Decode a packed string using BISE.
 *
 * Note that BISE input strings are not a whole number of bytes in length, and ASTC can start
 * strings at arbitrary bit offsets in the encoded data.
 *
 * @param         quant_level      The BISE alphabet size.
 * @param         character_count  The number of characters in the string.
 * @param         input_data       The packed string.
 * @param[in,out] output_data      The output storage, one byte per character.
 * @param         bit_offset       The starting offset in the output storage.
 */
void decode_ise(
	quant_method quant_level,
	unsigned int character_count,
	const uint8_t* input_data,
	uint8_t* output_data,
	unsigned int bit_offset);

/**
 * @brief Return the number of bits needed to encode an ISE sequence.
 *
 * This implementation assumes that the @c quant level is untrusted, given it may come from random
 * data being decompressed, so we return an arbitrary unencodable size if that is the case.
 *
 * @param character_count   The number of items in the sequence.
 * @param quant_level       The desired quantization level.
 *
 * @return The number of bits needed to encode the BISE string.
 */
unsigned int get_ise_sequence_bitcount(
	unsigned int character_count,
	quant_method quant_level);

/* ============================================================================
  Functionality for managing color partitioning.
============================================================================ */

/**
 * @brief Compute averages and dominant directions for each partition in a 2 component texture.
 *
 * @param      pi           The partition info for the current trial.
 * @param      blk          The image block color data to be compressed.
 * @param      ewb          The image block weighted error data.
 * @param      component1   The first component included in the analysis.
 * @param      component2   The second component included in the analysis.
 * @param[out] pm           The output partition metrics.
 *                          - Only pi.partition_count array entries actually get initialized.
 *                          - Direction vectors @c pm.dir are not normalized.
 */
void compute_avgs_and_dirs_2_comp(
	const partition_info& pi,
	const image_block& blk,
	const error_weight_block& ewb,
	unsigned int component1,
	unsigned int component2,
	partition_metrics pm[BLOCK_MAX_PARTITIONS]);

/**
 * @brief Compute averages and dominant directions for each partition in a 3 component texture.
 *
 * @param      pi                  The partition info for the current trial.
 * @param      blk                 The image block color data to be compressed.
 * @param      ewb                 The image block weighted error data.
 * @param      omitted_component   The component excluded from the analysis.
 * @param[out] pm                  The output partition metrics.
 *                                 - Only pi.partition_count array entries actually get initialized.
 *                                 - Direction vectors @c pm.dir are not normalized.
 */
void compute_avgs_and_dirs_3_comp(
	const partition_info& pi,
	const image_block& blk,
	const error_weight_block& ewb,
	unsigned int omitted_component,
	partition_metrics pm[BLOCK_MAX_PARTITIONS]);

/**
 * @brief Compute averages and dominant directions for each partition in a 3 component texture.
 *
 * This is a specialization of @c compute_avgs_and_dirs_3_comp where the omitted component is
 * always alpha, a common case during partition search.
 *
 * @param      pi                  The partition info for the current trial.
 * @param      blk                 The image block color data to be compressed.
 * @param      ewb                 The image block weighted error data.
 * @param[out] pm                  The output partition metrics.
 *                                 - Only pi.partition_count array entries actually get initialized.
 *                                 - Direction vectors @c pm.dir are not normalized.
 */
void compute_avgs_and_dirs_3_comp_rgb(
	const partition_info& pi,
	const image_block& blk,
	const error_weight_block& ewb,
	partition_metrics pm[BLOCK_MAX_PARTITIONS]);

/**
 * @brief Compute averages and dominant directions for each partition in a 4 component texture.
 *
 * @param      pi    The partition info for the current trial.
 * @param      blk   The image block color data to be compressed.
 * @param      ewb   The image block weighted error data.
 * @param[out] pm    The output partition metrics.
 *                   - Only pi.partition_count array entries actually get initialized.
 *                   - Direction vectors @c pm.dir are not normalized.
 */
void compute_avgs_and_dirs_4_comp(
	const partition_info& pi,
	const image_block& blk,
	const error_weight_block& ewb,
	partition_metrics pm[BLOCK_MAX_PARTITIONS]);

/**
 * @brief Compute the RGB error for uncorrelated and same chroma projections.
 *
 * The output of compute averages and dirs is post processed to define two lines, both of which go
 * through the mean-color-value.  One line has a direction defined by the dominant direction; this
 * is used to assess the error from using an uncorrelated color representation. The other line goes
 * through (0,0,0) and is used to assess the error from using an RGBS color representation.
 *
 * This function computes the squared error when using these two representations.
 *
 * @param         pi              The partition info for the current trial.
 * @param         blk             The image block color data to be compressed.
 * @param         ewb             The image block weighted error data.
 * @param[in,out] plines          Processed line inputs, and line length outputs.
 * @param[out]    uncor_error     The cumulative error for using the uncorrelated line.
 * @param[out]    samec_error     The cumulative error for using the same chroma line.
 */
void compute_error_squared_rgb(
	const partition_info& pi,
	const image_block& blk,
	const error_weight_block& ewb,
	partition_lines3 plines[BLOCK_MAX_PARTITIONS],
	float& uncor_error,
	float& samec_error);

/**
 * @brief Compute the RGBA error for uncorrelated and same chroma projections.
 *
 * The output of compute averages and dirs is post processed to define two lines, both of which go
 * through the mean-color-value.  One line has a direction defined by the dominant direction; this
 * is used to assess the error from using an uncorrelated color representation. The other line goes
 * through (0,0,0,1) and is used to assess the error from using an RGBS color representation.
 *
 * This function computes the squared error when using these two representations.
 *
 * @param      pi              The partition info for the current trial.
 * @param      blk             The image block color data to be compressed.
 * @param      ewb             The image block weighted error data.
 * @param      uncor_plines    Processed uncorrelated partition lines for each partition.
 * @param      samec_plines    Processed same chroma partition lines for each partition.
 * @param[out] uncor_lengths   The length of each components deviation from the line.
 * @param[out] samec_lengths   The length of each components deviation from the line.
 * @param[out] uncor_error     The cumulative error for using the uncorrelated line.
 * @param[out] samec_error     The cumulative error for using the same chroma line.
 */
void compute_error_squared_rgba(
	const partition_info& pi,
	const image_block& blk,
	const error_weight_block& ewb,
	const processed_line4 uncor_plines[BLOCK_MAX_PARTITIONS],
	const processed_line4 samec_plines[BLOCK_MAX_PARTITIONS],
	float uncor_lengths[BLOCK_MAX_PARTITIONS],
	float samec_lengths[BLOCK_MAX_PARTITIONS],
	float& uncor_error,
	float& samec_error);

/**
 * @brief Find the best set of partitions to trial for a given block.
 *
 * On return @c best_partition_uncor contains the best partition  assuming data has uncorrelated
 * chroma, @c best_partition_samec contains the best partition assuming data has corelated chroma.
 *
 * @param      bsd                        The block size information.
 * @param      blk                        The image block color data to compress.
 * @param      ewb                        The image block weighted error data.
 * @param      partition_count            The number of partitions in the block.
 * @param      partition_search_limit     The number of candidate partition encodings to trial.
 * @param[out] best_partition_uncor       The best partition for uncorrelated chroma.
 * @param[out] best_partition_samec       The best partition for correlated chroma.
 */
void find_best_partition_candidates(
	const block_size_descriptor& bsd,
	const image_block& blk,
	const error_weight_block& ewb,
	unsigned int partition_count,
	unsigned int partition_search_limit,
	unsigned int& best_partition_uncor,
	unsigned int& best_partition_samec);

/* ============================================================================
  Functionality for managing images and image related data.
============================================================================ */

/**
 * @brief Setup computation of regional averages and variances in an image.
 *
 * This must be done by only a single thread per image, before any thread calls
 * @c compute_averages_and_variances().
 *
 * Results are written back into @c img->input_averages, @c img->input_variances,
 * and @c img->input_alpha_averages.
 *
 * @param      img                     The input image data, also holds output data.
 * @param      rgb_power               The RGB component power.
 * @param      alpha_power             The A component power.
 * @param      avg_var_kernel_radius   The kernel radius (in pixels) for avg and var.
 * @param      alpha_kernel_radius     The kernel radius (in pixels) for alpha mods.
 * @param      swz                     Input data component swizzle.
 * @param[out] ag                      The average variance arguments to init.
 *
 * @return The number of tasks in the processing stage.
 */
unsigned int init_compute_averages_and_variances(
	const astcenc_image& img,
	float rgb_power,
	float alpha_power,
	unsigned int avg_var_kernel_radius,
	unsigned int alpha_kernel_radius,
	const astcenc_swizzle& swz,
	avg_var_args& ag);

/**
 * @brief Compute regional averages and variances.
 *
 * This function can be called by multiple threads, but only after a single thread calls the setup
 * function @c init_compute_averages_and_variances().
 *
 * Results are written back into @c img->input_averages, @c img->input_variances,
 * and @c img->input_alpha_averages.
 *
 * @param[out] ctx   The context.
 * @param      ag    The average and variance arguments created during setup.
 */
void compute_averages_and_variances(
	astcenc_context& ctx,
	const avg_var_args& ag);

/**
 * @brief Fetch a single image block from the input image
 *
 * @param      decode_mode   The compression color profile.
 * @param      img           The input image data.
 * @param[out] blk           The image block to populate.
 * @param      bsd           The block size information.
 * @param      xpos          The block X coordinate in the input image.
 * @param      ypos          The block Y coordinate in the input image.
 * @param      zpos          The block Z coordinate in the input image.
 * @param      swz           The swizzle to apply on load.
 */
void fetch_image_block(
	astcenc_profile decode_mode,
	const astcenc_image& img,
	image_block& blk,
	const block_size_descriptor& bsd,
	unsigned int xpos,
	unsigned int ypos,
	unsigned int zpos,
	const astcenc_swizzle& swz);

/**
 * @brief Write a single image block from the output image
 *
 * @param[out] img           The input image data.
 * @param      blk           The image block to populate.
 * @param      bsd           The block size information.
 * @param      xpos          The block X coordinate in the input image.
 * @param      ypos          The block Y coordinate in the input image.
 * @param      zpos          The block Z coordinate in the input image.
 * @param      swz           The swizzle to apply on store.
 */
void write_image_block(
	astcenc_image& img,
	const image_block& blk,
	const block_size_descriptor& bsd,
	unsigned int xpos,
	unsigned int ypos,
	unsigned int zpos,
	const astcenc_swizzle& swz);

/* ============================================================================
  Functionality for computing endpoint colors and weights for a block.
============================================================================ */

/**
 * @brief Compute ideal endpoint colors and weights for 1 plane of weights.
 *
 * The ideal endpoints define a color line for the partition. For each texel the ideal weight
 * defines an exact position on the partition color line. We can then use these to assess the error
 * introduced by removing and quantizing the weight grid.
 *
 * @param      bsd   The block size information.
 * @param      blk   The image block color data to compress.
 * @param      ewb   The image block weighted error data.
 * @param      pi    The partition info for the current trial.
 * @param[out] ei    The endpoint and weight values.
 */
void compute_ideal_colors_and_weights_1plane(
	const block_size_descriptor& bsd,
	const image_block& blk,
	const error_weight_block& ewb,
	const partition_info& pi,
	endpoints_and_weights& ei);

/**
 * @brief Compute ideal endpoint colors and weights for 2 planes of weights.
 *
 * The ideal endpoints define a color line for the partition. For each texel the ideal weight
 * defines an exact position on the partition color line. We can then use these to assess the error
 * introduced by removing and quantizing the weight grid.
 *
 * @param      bsd                The block size information.
 * @param      blk                The image block color data to compress.
 * @param      ewb                The image block weighted error data.
 * @param      plane2_component   The component assigned to plane 2.
 * @param[out] ei1                The endpoint and weight values for plane 1.
 * @param[out] ei2                The endpoint and weight values for plane 2.
 */
void compute_ideal_colors_and_weights_2planes(
	const block_size_descriptor& bsd,
	const image_block& blk,
	const error_weight_block& ewb,
	unsigned int plane2_component,
	endpoints_and_weights& ei1,
	endpoints_and_weights& ei2);

/**
 * @brief Compute the optimal unquantized weights for a decimation table.
 *
 * After computing ideal weights for the case for a complete weight grid, we we want to compute the
 * ideal weights for the case where weights exist only for some texels. We do this with a
 * steepest-descent grid solver which works as follows:
 *
 * First, for each actual weight, perform a weighted averaging of the texels affected by the weight.
 * Then, set step size to <some initial value> and attempt one step towards the original ideal
 * weight if it helps to reduce error.
 *
 * @param      eai_in                   The non-decimated endpoints and weights.
 * @param      eai_out                  A copy of eai_in we can modify later for refinement.
 * @param      di                       The selected weight decimation.
 * @param[out] dec_weight_ideal_value   The ideal values for the decimated weight set.
 * @param[out] dec_weight_ideal_sig     The significance of each weight in the decimated weight_set.
 */
void compute_ideal_weights_for_decimation(
	const endpoints_and_weights& eai_in,
	endpoints_and_weights& eai_out,
	const decimation_info& di,
	float* dec_weight_ideal_value,
	float* dec_weight_ideal_sig);

/**
 * @brief Compute the optimal quantized weights for a decimation table.
 *
 * We test the two closest weight indices in the allowed quantization range and keep the weight that
 * is the closest match.
 *
 * @param      di                        The selected weight decimation.
 * @param      low_bound                 The lowest weight allowed.
 * @param      high_bound                The highest weight allowed.
 * @param      dec_weight_ideal_value    The ideal weight set.
 * @param[out] dec_weight_quant_uvalue   The output quantized weight as a float.
 * @param[out] dec_weight_quant_pvalue   The output quantized weight as encoded int.
 * @param      quant_level               The desired weight quant level.
 */
void compute_quantized_weights_for_decimation(
	const decimation_info& di,
	float low_bound,
	float high_bound,
	const float* dec_weight_ideal_value,
	float* dec_weight_quant_uvalue,
	uint8_t* dec_weight_quant_pvalue,
	quant_method quant_level);

/**
 * @brief Compute the infilled weight for a texel index in a decimated grid.
 *
 * @param di        The weight grid decimation to use.
 * @param weights   The decimated weight values to use.
 * @param index     The texel index to interpolate.
 *
 * @return The interpolated weight for the given texel.
 */
static inline float bilinear_infill(
	const decimation_info& di,
	const float* weights,
	unsigned int index
) {
	return (weights[di.texel_weights_4t[0][index]] * di.texel_weights_float_4t[0][index] +
	        weights[di.texel_weights_4t[1][index]] * di.texel_weights_float_4t[1][index]) +
	       (weights[di.texel_weights_4t[2][index]] * di.texel_weights_float_4t[2][index] +
	        weights[di.texel_weights_4t[3][index]] * di.texel_weights_float_4t[3][index]);
}

/**
 * @brief Compute the infilled weight for N texel indices in a decimated grid.
 *
 * @param di        The weight grid decimation to use.
 * @param weights   The decimated weight values to use.
 * @param index     The first texel index to interpolate.
 *
 * @return The interpolated weight for the given set of SIMD_WIDTH texels.
 */
static inline vfloat bilinear_infill_vla(
	const decimation_info& di,
	const float* weights,
	unsigned int index
) {
	// Load the bilinear filter texel weight indexes in the decimated grid
	vint weight_idx0 = vint(di.texel_weights_4t[0] + index);
	vint weight_idx1 = vint(di.texel_weights_4t[1] + index);
	vint weight_idx2 = vint(di.texel_weights_4t[2] + index);
	vint weight_idx3 = vint(di.texel_weights_4t[3] + index);

	// Load the bilinear filter weights from the decimated grid
	vfloat weight_val0 = gatherf(weights, weight_idx0);
	vfloat weight_val1 = gatherf(weights, weight_idx1);
	vfloat weight_val2 = gatherf(weights, weight_idx2);
	vfloat weight_val3 = gatherf(weights, weight_idx3);

	// Load the weight contribution factors for each decimated weight
	vfloat tex_weight_float0 = loada(di.texel_weights_float_4t[0] + index);
	vfloat tex_weight_float1 = loada(di.texel_weights_float_4t[1] + index);
	vfloat tex_weight_float2 = loada(di.texel_weights_float_4t[2] + index);
	vfloat tex_weight_float3 = loada(di.texel_weights_float_4t[3] + index);

	// Compute the bilinear interpolation to generate the per-texel weight
	return (weight_val0 * tex_weight_float0 + weight_val1 * tex_weight_float1) +
	       (weight_val2 * tex_weight_float2 + weight_val3 * tex_weight_float3);
}

/**
 * @brief Compute the error of a decimated weight set for 1 plane.
 *
 * After computing ideal weights for the case with one weight per texel, we want to compute the
 * error for decimated weight grids where weights are stored at a lower resolution. This function
 * computes the error of the reduced grid, compared to the full grid.
 *
 * @param eai                       The ideal weights for the full grid.
 * @param di                        The selected weight decimation.
 * @param dec_weight_quant_uvalue   The quantized weights for the decimated grid.
 *
 * @return The accumulated error.
 */
float compute_error_of_weight_set_1plane(
	const endpoints_and_weights& eai,
	const decimation_info& di,
	const float* dec_weight_quant_uvalue);

/**
 * @brief Compute the error of a decimated weight set for 2 planes.
 *
 * After computing ideal weights for the case with one weight per texel, we want to compute the
 * error for decimated weight grids where weights are stored at a lower resolution. This function
 * computes the error of the reduced grid, compared to the full grid.
 *
 * @param eai1                             The ideal weights for the full grid and plane 1.
 * @param eai2                             The ideal weights for the full grid and plane 2.
 * @param di                               The selected weight decimation.
 * @param dec_weight_quant_uvalue_plane1   The quantized weights for the decimated grid plane 1.
 * @param dec_weight_quant_uvalue_plane2   The quantized weights for the decimated grid plane 2.
 *
 * @return The accumulated error.
 */
float compute_error_of_weight_set_2planes(
	const endpoints_and_weights& eai1,
	const endpoints_and_weights& eai2,
	const decimation_info& di,
	const float* dec_weight_quant_uvalue_plane1,
	const float* dec_weight_quant_uvalue_plane2);

/**
 * @brief Pack a single pair of color endpoints as effectively as possible.
 *
 * The user requests a base color endpoint mode in @c format, but the quantizer may choose a
 * delta-based representation. It will report back the format variant it actually used.
 *
 * @param      color0       The input unquantized color0 endpoint for absolute endpoint pairs.
 * @param      color1       The input unquantized color1 endpoint for absolute endpoint pairs.
 * @param      rgbs_color   The input unquantized RGBS variant endpoint for same chroma endpoints.
 * @param      rgbo_color   The input unquantized RGBS variant endpoint for HDR endpoints..
 * @param      format       The desired base format.
 * @param[out] output       The output storage for the quantized colors/
 * @param      quant_level  The quantization level requested.
 *
 * @return The actual endpoint mode used.
 */
uint8_t pack_color_endpoints(
	vfloat4 color0,
	vfloat4 color1,
	vfloat4 rgbs_color,
	vfloat4 rgbo_color,
	int format,
	uint8_t* output,
	quant_method quant_level);

/**
 * @brief Unpack a single pair of encoded and quantized color endpoints.
 *
 * @param      decode_mode   The decode mode (LDR, HDR).
 * @param      format        The color endpoint mode used.
 * @param      quant_level   The quantization level used.
 * @param      input         The raw array of encoded input integers. The length of this array
 *                           depends on @c format; it can be safely assumed to be large enough.
 * @param[out] rgb_hdr       Is the endpoint using HDR for the RGB channels?
 * @param[out] alpha_hdr     Is the endpoint using HDR for the A channel?
 * @param[out] output0       The output color for endpoint 0.
 * @param[out] output1       The output color for endpoint 1.
 */
void unpack_color_endpoints(
	astcenc_profile decode_mode,
	int format,
	quant_method quant_level,
	const uint8_t* input,
	bool& rgb_hdr,
	bool& alpha_hdr,
	vint4& output0,
	vint4& output1);

/**
 * @brief Unpack a set of quantized and decimated weights.
 *
 * @param      bsd              The block size information.
 * @param      scb              The symbolic compressed encoding.
 * @param      di               The weight grid decimation table.
 * @param      is_dual_plane    @c true if this is a dual plane block, @c false otherwise.
 * @param      quant_level      The weight quantization level.
 * @param[out] weights_plane1   The output array for storing the plane 1 weights.
 * @param[out] weights_plane2   The output array for storing the plane 2 weights.
 */
void unpack_weights(
	const block_size_descriptor& bsd,
	const symbolic_compressed_block& scb,
	const decimation_info& di,
	bool is_dual_plane,
	quant_method quant_level,
	int weights_plane1[BLOCK_MAX_TEXELS],
	int weights_plane2[BLOCK_MAX_TEXELS]);

/**
 * @brief Identify, for each mode, which set of color endpoint produces the best result.
 *
 * Returns the best @c tune_candidate_limit best looking modes, along with the ideal color encoding
 * combination for each. The modified quantization level can be used when all formats are the same,
 * as this frees up two additional bits of storage.
 *
 * @param      bsd                           The block size information.
 * @param      pi                            The partition info for the current trial.
 * @param      blk                           The image block color data to compress.
 * @param      ewb                           The image block weighted error data.
 * @param      ep                            The ideal endpoints.
 * @param      qwt_bitcounts                 Bit counts for different quantization methods.
 * @param      qwt_errors                    Errors for different quantization methods.
 * @param      tune_candidate_limit          The max number of candidates to return, may be less.
 * @param[out] partition_format_specifiers   The best formats per partition.
 * @param[out] block_mode                    The best packed block mode indexes.
 * @param[out] quant_level                   The best color quant level.
 * @param[out] quant_level_mod               The best color quant level if endpoints are the same.
 *
 * @return The actual number of candidate matches returned.
 */
unsigned int compute_ideal_endpoint_formats(
	const block_size_descriptor& bsd,
	const partition_info& pi,
	const image_block& blk,
	const error_weight_block& ewb,
	const endpoints& ep,
	const int* qwt_bitcounts,
	const float* qwt_errors,
	unsigned int tune_candidate_limit,
	int partition_format_specifiers[TUNE_MAX_TRIAL_CANDIDATES][BLOCK_MAX_PARTITIONS],
	int block_mode[TUNE_MAX_TRIAL_CANDIDATES],
	quant_method quant_level[TUNE_MAX_TRIAL_CANDIDATES],
	quant_method quant_level_mod[TUNE_MAX_TRIAL_CANDIDATES]);

/**
 * @brief For a given 1 plane weight set recompute the endpoint colors.
 *
 * As we quantize and decimate weights the optimal endpoint colors may change slightly, so we must
 * recompute the ideal colors for a specific weight set.
 *
 * @param         blk                        The image block color data to compress.
 * @param         ewb                        The image block weighted error data.
 * @param         pi                         The partition info for the current trial.
 * @param         di                         The weight grid decimation table.
 * @param         weight_quant_mode          The weight grid quantization level.
 * @param         dec_weights_quant_pvalue   The quantized weight set.
 * @param[in,out] ep                         The color endpoints (modifed in place).
 * @param[out]    rgbs_vectors               The RGB+scale vectors for LDR blocks.
 * @param[out]    rgbo_vectors               The RGB+offset vectors for HDR blocks.
 */
void recompute_ideal_colors_1plane(
	const image_block& blk,
	const error_weight_block& ewb,
	const partition_info& pi,
	const decimation_info& di,
	int weight_quant_mode,
	const uint8_t* dec_weights_quant_pvalue,
	endpoints& ep,
	vfloat4 rgbs_vectors[BLOCK_MAX_PARTITIONS],
	vfloat4 rgbo_vectors[BLOCK_MAX_PARTITIONS]);

/**
 * @brief For a given 2 plane weight set recompute the endpoint colors.
 *
 * As we quantize and decimate weights the optimal endpoint colors may change slightly, so we must
 * recompute the ideal colors for a specific weight set.
 *
 * @param         blk                               The image block color data to compress.
 * @param         ewb                               The image block weighted error data.
 * @param         bsd                               The block_size descriptor.
 * @param         di                                The weight grid decimation table.
 * @param         weight_quant_mode                 The weight grid quantization level.
 * @param         dec_weights_quant_pvalue_plane1   The quantized weight set for plane 1.
 * @param         dec_weights_quant_pvalue_plane2   The quantized weight set for plane 2.
 * @param[in,out] ep                                The color endpoints (modifed in place).
 * @param[out]    rgbs_vector                       The RGB+scale color for LDR blocks.
 * @param[out]    rgbo_vector                       The RGB+offset color for HDR blocks.
 * @param         plane2_component                  The component assigned to plane 2.
 */
void recompute_ideal_colors_2planes(
	const image_block& blk,
	const error_weight_block& ewb,
	const block_size_descriptor& bsd,
	const decimation_info& di,
	int weight_quant_mode,
	const uint8_t* dec_weights_quant_pvalue_plane1,
	const uint8_t* dec_weights_quant_pvalue_plane2,
	endpoints& ep,
	vfloat4& rgbs_vector,
	vfloat4& rgbo_vector,
	int plane2_component);

/**
 * @brief Expand the deblock weights based on the config deblocking parameter.
 *
 * The approach to deblocking is a general purpose approach which elevates the error weight
 * significance of texels closest to the block periphery. This function computes the deblock weights
 * for each texel, which can be mixed on a block-by-block basis with the other error weighting
 * parameters to compute a specific per-texel weight for a trial.
 *
 * @param[in,out] ctx   The context to expand.
 */
void expand_deblock_weights(
	astcenc_context& ctx);

/**
 * @brief Expand the angular tables needed for the alternative to PCA that we use.
 */
void prepare_angular_tables();

/**
 * @brief Compute the angular endpoints for one plane for each block mode.
 *
 * @param      tune_low_weight_limit     Weight count cutoff below which we use simpler searches.
 * @param      only_always               Only consider block modes that are always enabled.
 * @param      bsd                       The block size descriptor for the current trial.
 * @param      dec_weight_quant_uvalue   The decimated and quantized weight values.
 * @param      dec_weight_quant_sig      The significance of each weight.
 * @param[out] low_value                 The lowest weight to consider for each block mode.
 * @param[out] high_value                The highest weight to consider for each block mode.
 */
void compute_angular_endpoints_1plane(
	unsigned int tune_low_weight_limit,
	bool only_always,
	const block_size_descriptor& bsd,
	const float* dec_weight_quant_uvalue,
	const float* dec_weight_quant_sig,
	float low_value[WEIGHTS_MAX_BLOCK_MODES],
	float high_value[WEIGHTS_MAX_BLOCK_MODES]);

/**
 * @brief Compute the angular endpoints for two planes for each block mode.
 *
 * @param      tune_low_weight_limit    Weight count cutoff below which we use simpler searches.
 * @param     bsd                       The block size descriptor for the current trial.
 * @param     dec_weight_quant_uvalue   The decimated and quantized weight values.
 * @param     dec_weight_quant_sig      The significance of each weight.
 * @param[out] low_value1               The lowest weight p1 to consider for each block mode.
 * @param[out] high_value1              The highest weight p1 to consider for each block mode.
 * @param[out] low_value2               The lowest weight p2 to consider for each block mode.
 * @param[out] high_value2              The highest weight p2 to consider for each block mode.
 */
void compute_angular_endpoints_2planes(
	unsigned int tune_low_weight_limit,
	const block_size_descriptor& bsd,
	const float* dec_weight_quant_uvalue,
	const float* dec_weight_quant_sig,
	float low_value1[WEIGHTS_MAX_BLOCK_MODES],
	float high_value1[WEIGHTS_MAX_BLOCK_MODES],
	float low_value2[WEIGHTS_MAX_BLOCK_MODES],
	float high_value2[WEIGHTS_MAX_BLOCK_MODES]);

/* ============================================================================
  Functionality for high level compression and decompression access.
============================================================================ */

/**
 * @brief Compress an image block into a physical block.
 *
 * @param      ctx      The compressor context and configuration.
 * @param      image    The input image information.
 * @param      blk      The image block color data to compress.
 * @param[out] pcb      The physical compressed block output.
 * @param[out] tmpbuf   Preallocated scratch buffers for the compressor.
 */
void compress_block(
	const astcenc_context& ctx,
	const astcenc_image& image,
	const image_block& blk,
	physical_compressed_block& pcb,
	compression_working_buffers& tmpbuf);

/**
 * @brief Decompress a symbolic block in to an image block.
 *
 * @param      decode_mode   The decode mode (LDR, HDR, etc).
 * @param      bsd           The block size information.
 * @param      xpos          The X coordinate of the block in the overall image.
 * @param      ypos          The Y coordinate of the block in the overall image.
 * @param      zpos          The Z coordinate of the block in the overall image.
 * @param[out] blk           The decompressed image block color data.
 */
void decompress_symbolic_block(
	astcenc_profile decode_mode,
	const block_size_descriptor& bsd,
	int xpos,
	int ypos,
	int zpos,
	const symbolic_compressed_block& scb,
	image_block& blk);

/**
 * @brief Compute the error between a symbolic block and the original input data.
 *
 * In RGBM mode this will reject blocks that attempt to encode a zero M value.
 *
 * @param config   The compressor config.
 * @param bsd      The block size information.
 * @param scb      The symbolic compressed encoding.
 * @param blk      The original image block color data.
 * @param ewb      The error weight block data.
 *
 * @return Returns the computed error, or a negative value if the encoding
 *         should be rejected for any reason.
 */
float compute_symbolic_block_difference(
	const astcenc_config& config,
	const block_size_descriptor& bsd,
	const symbolic_compressed_block& scb,
	const image_block& blk,
	const error_weight_block& ewb) ;

/**
 * @brief Convert a symbolic representation into a binary physical encoding.
 *
 * It is assumed that the symbolic encoding is valid and encodable, or
 * previously flagged as an error block if an error color it to be encoded.
 *
 * @param      bsd   The block size information.
 * @param      scb   The symbolic representation.
 * @param[out] pcb   The binary encoded data.
 */
void symbolic_to_physical(
	const block_size_descriptor& bsd,
	const symbolic_compressed_block& scb,
	physical_compressed_block& pcb);

/**
 * @brief Convert a binary physical encoding into a symbolic representation.
 *
 * This function can cope with arbitrary input data; output blocks will be
 * flagged as an error block if the encoding is invalid.
 *
 * @param      bsd   The block size information.
 * @param      pcb   The binary encoded data.
 * @param[out] scb   The output symbolic representation.
 */
void physical_to_symbolic(
	const block_size_descriptor& bsd,
	const physical_compressed_block& pcb,
	symbolic_compressed_block& scb);

/* ============================================================================
Platform-specific functions.
============================================================================ */
/**
 * @brief Run-time detection if the host CPU supports the POPCNT extension.
 *
 * @return @c true if supported, @c false if not.
 */
bool cpu_supports_popcnt();

/**
 * @brief Run-time detection if the host CPU supports F16C extension.
 *
 * @return @c true if supported, @c false if not.
 */
bool cpu_supports_f16c();

/**
 * @brief Run-time detection if the host CPU supports SSE 4.1 extension.
 *
 * @return @c true if supported, @c false if not.
 */
bool cpu_supports_sse41();

/**
 * @brief Run-time detection if the host CPU supports AVX 2 extension.
 *
 * @return @c true if supported, @c false if not.
 */
bool cpu_supports_avx2();

/**
 * @brief Allocate an aligned memory buffer.
 *
 * Allocated memory must be freed by aligned_free;
 *
 * @param size    The desired buffer size.
 * @param align   The desired buffer alignment; must be 2^N.
 *
 * @return The memory buffer pointer or nullptr on allocation failure.
 */
template<typename T>
T* aligned_malloc(size_t size, size_t align)
{
	void* ptr;
	int error = 0;

#if defined(_WIN32)
	ptr = _aligned_malloc(size, align);
#else
	error = posix_memalign(&ptr, align, size);
#endif

	if (error || (!ptr))
	{
		return nullptr;
	}

	return static_cast<T*>(ptr);
}

/**
 * @brief Free an aligned memory buffer.
 *
 * @param ptr   The buffer to free.
 */
template<typename T>
void aligned_free(T* ptr)
{
#if defined(_WIN32)
	_aligned_free((void*)ptr);
#else
	free((void*)ptr);
#endif
}

#endif
