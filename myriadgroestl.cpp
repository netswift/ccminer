#include <string.h>
#include <stdint.h>
#include <cuda_runtime.h>
#include <openssl/sha.h>

#include "sph/sph_groestl.h"

#include "miner.h"

void myriadgroestl_cpu_init(int thr_id, uint32_t threads);
void myriadgroestl_cpu_free(int thr_id);
void myriadgroestl_cpu_setBlock(int thr_id, void *data, void *pTargetIn);
void myriadgroestl_cpu_hash(int thr_id, uint32_t threads, uint32_t startNounce, void *outputHashes, uint32_t *nounce);

void myriadhash(void *state, const void *input)
{
	uint32_t _ALIGN(64) hash[16];
	sph_groestl512_context ctx_groestl;
	SHA256_CTX sha256;

	sph_groestl512_init(&ctx_groestl);
	sph_groestl512(&ctx_groestl, input, 80);
	sph_groestl512_close(&ctx_groestl, hash);

	SHA256_Init(&sha256);
	SHA256_Update(&sha256,(unsigned char *)hash, 64);
	SHA256_Final((unsigned char *)hash, &sha256);

	memcpy(state, hash, 32);
}

static bool init[MAX_GPUS] = { 0 };

int scanhash_myriad(int thr_id, struct work *work, uint32_t max_nonce, unsigned long *hashes_done)
{
	uint32_t _ALIGN(64) endiandata[32];
	uint32_t *pdata = work->data;
	uint32_t *ptarget = work->target;
	uint32_t start_nonce = pdata[19];
	uint32_t throughput = cuda_default_throughput(thr_id, 1U << 17);
	if (init[thr_id]) throughput = min(throughput, max_nonce - start_nonce);

	uint32_t *outputHash = (uint32_t*)malloc(throughput * 64);

	if (opt_benchmark)
		((uint32_t*)ptarget)[7] = 0x0000ff;

	// init
	if(!init[thr_id])
	{
		cudaSetDevice(device_map[thr_id]);
		myriadgroestl_cpu_init(thr_id, throughput);
		init[thr_id] = true;
	}

	for (int k=0; k < 20; k++)
		be32enc(&endiandata[k], pdata[k]);

	// Context mit dem Endian gedrehten Blockheader vorbereiten (Nonce wird sp�ter ersetzt)
	myriadgroestl_cpu_setBlock(thr_id, endiandata, (void*)ptarget);

	do {
		// GPU
		uint32_t foundNounce = UINT32_MAX;

		*hashes_done = pdata[19] - start_nonce + throughput;

		myriadgroestl_cpu_hash(thr_id, throughput, pdata[19], outputHash, &foundNounce);

		if (foundNounce < UINT32_MAX)
		{
			uint32_t _ALIGN(64) vhash[8];
			endiandata[19] = swab32(foundNounce);
			myriadhash(vhash, endiandata);
			if (vhash[7] <= ptarget[7] && fulltest(vhash, ptarget)) {
				work_set_target_ratio(work, vhash);
				pdata[19] = foundNounce;
				free(outputHash);
				return 1;
			} else {
				gpulog(LOG_WARNING, thr_id, "result for %08x does not validate on CPU!", foundNounce);
			}
		}

		if ((uint64_t) pdata[19] + throughput > max_nonce) {
			*hashes_done = pdata[19] - start_nonce;
			pdata[19] = max_nonce;
			break;
		}
		pdata[19] += throughput;

	} while (!work_restart[thr_id].restart);

	free(outputHash);
	return 0;
}

// cleanup
void free_myriad(int thr_id)
{
	if (!init[thr_id])
		return;

	cudaThreadSynchronize();

	myriadgroestl_cpu_free(thr_id);
	init[thr_id] = false;

	cudaDeviceSynchronize();
}
