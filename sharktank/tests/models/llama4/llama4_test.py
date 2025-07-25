import transformers.models
from sharktank.utils.testing import TempDirTestBase
from sharktank.models.llama4.testing import (
    make_toy_model_config,
    config_to_hugging_face_text_config,
    theta_to_hugging_face_state_dict,
)
from sharktank.models.llama.testing import make_random_llama_theta
from sharktank.models.llm import PagedLlmModelV1
import transformers
import torch
import pytest
from sharktank.utils.testing import is_mi300x, IreeVsEagerLLMTester, is_cpu_condition
import random
from parameterized import parameterized
import os


def convert_hf_2D_input_mask_to_4D_attention_mask(
    mask: torch.Tensor, model: PagedLlmModelV1
) -> torch.Tensor:
    inverted_mask = mask == 0
    return model.attention_mask(inverted_mask)


class Llama4Test(TempDirTestBase):
    def setUp(self):
        super().setUp()
        torch.random.manual_seed(12345)

    @pytest.mark.xfail(
        is_mi300x,
        raises=TypeError,
        strict=False,
        reason="argument of type 'NoneType' is not iterable",
    )
    def testCompareToyEagerVsHuggingFace(self):
        dtype = torch.float32
        torch.set_printoptions(
            linewidth=120, threshold=1000, edgeitems=4, precision=2, sci_mode=True
        )
        config = make_toy_model_config(dtype=dtype)
        theta = make_random_llama_theta(config, dtype_rest=dtype, dtype_norm=dtype)
        hf_config = config_to_hugging_face_text_config(config)

        model = PagedLlmModelV1(theta=theta, config=config)
        hf_model = transformers.models.llama4.Llama4ForCausalLM(hf_config)

        orig_state_dict = hf_model.state_dict()
        hf_state_dict = theta_to_hugging_face_state_dict(theta, config)
        hf_model.load_state_dict(hf_state_dict)

        batch_size = 41
        batch_seq_len = config.hp.context_length
        input_ids = torch.randint(
            low=0,
            high=config.hp.vocab_size,
            size=[batch_size, batch_seq_len],
            dtype=torch.long,
        )

        # We need to create the cache ourselves as HF would create it always in bf16.
        hf_past_key_values = transformers.cache_utils.HybridChunkedCache(
            hf_config,
            max_batch_size=input_ids.shape[0],
            max_cache_len=input_ids.shape[1],
            dtype=dtype,
        )

        hf_2d_attention_mask = torch.randint_like(input_ids, low=0, high=2)
        attention_mask = convert_hf_2D_input_mask_to_4D_attention_mask(
            mask=hf_2d_attention_mask, model=model
        )

        @torch.compiler.disable(recursive=True)
        def run_hf_model():
            return hf_model(
                input_ids=input_ids,
                attention_mask=hf_2d_attention_mask,
                past_key_values=hf_past_key_values,
            )

        hf_output = run_hf_model()

        page_count = (len(input_ids[0]) // config.block_seq_stride) * batch_size
        kv_cache_state = model.cache.allocate(page_count)
        seq_block_ids = torch.arange(
            start=0, end=input_ids.numel() // config.block_seq_stride, dtype=torch.long
        ).view(batch_size, batch_seq_len // config.block_seq_stride)

        output = model.prefill(
            tokens=input_ids,
            attention_mask=[attention_mask],
            cache_state=kv_cache_state,
            seq_block_ids=[seq_block_ids],
        )

        torch.testing.assert_close(hf_output.logits, output, atol=2e-4, rtol=2e-2)


@pytest.mark.usefixtures("iree_flags", "device")
@is_mi300x
class TestLlama4IreeEager(TempDirTestBase):
    def helper_run(self, dtype, atol, rtol):
        seed = 1234
        random.seed(seed)
        torch.manual_seed(seed)
        config = make_toy_model_config(dtype=dtype)
        theta = make_random_llama_theta(
            config=config, dtype_rest=dtype, dtype_norm=dtype
        )

        tester = IreeVsEagerLLMTester(
            work_dir=self._temp_dir,
            theta=theta,
            config=config,
            torch_device=self.device,
            iree_device=self.iree_device,
            iree_hip_target=self.iree_hip_target,
            iree_hal_target_device=self.iree_hal_target_device,
            skip_decode=True,
            use_qk_norm=True,
            attention_chunk_size=37,
        )
        tester.run_and_compare_iree_vs_eager(atol=atol, rtol=rtol)

    @parameterized.expand(
        [
            (torch.float16, 1e-1, 1e-1),
        ]
    )
    @pytest.mark.xfail(
        condition=os.environ.get("PYTEST_ADDOPTS", "").find("--device=cpu") == -1,
        raises=AssertionError,
        strict=False,
        reason="Numerical diff on gpu; should move to cross entropy",
    )
    def testUnshardedToySizedModelIREEVsEager(self, dtype, atol, rtol):
        self.helper_run(dtype=dtype, atol=atol, rtol=rtol)
