#pragma once
#include <cstdint>
#include <cstddef>

namespace CemuPad {

/**
 * Cauchy Reed-Solomon erasure codec over GF(2^8) (polynomial 285 / 0x11D).
 *
 * Implements the exact same Cauchy generator matrix and Galois field arithmetic
 * as nanors (used in Apollo/Sunshine and Moonlight).
 */
class ReedSolomon
{
public:
	// Maximum data shards supported per frame block
	static constexpr int MAX_DATA_SHARDS = 255;
	static constexpr int MAX_PARITY_SHARDS = 64;

	/**
	 * Encodes [dataCount] data shards into [parityCount] parity shards.
	 *
	 * Each shard in [dataShards] and [parityShards] must point to a buffer
	 * of at least [blockSize] bytes.
	 *
	 * Returns true on success, false if parameters are invalid.
	 */
	static bool Encode(
		const uint8_t* const* dataShards,
		int dataCount,
		uint8_t* const* parityShards,
		int parityCount,
		int blockSize
	);
};

} // namespace CemuPad
