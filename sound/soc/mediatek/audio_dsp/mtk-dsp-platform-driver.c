// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2018 MediaTek Inc.

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/string.h>
#include <sound/soc.h>
#include <audio_task_manager.h>
#include <linux/spinlock.h>


#ifdef CONFIG_MTK_AUDIODSP_SUPPORT
#include <adsp_helper.h>
#else
#include <scp_helper.h>
#endif

#include "mtk-dsp-mem-control.h"
#include "mtk-base-dsp.h"
#include "mtk-dsp-common.h"
#include "mtk-dsp-platform-driver.h"
#include "mtk-base-afe.h"

static DEFINE_SPINLOCK(dsp_ringbuf_lock);

#define IPIMSG_SHARE_MEM (1024)
//#define DEBUG_VERBOSE
//#define DEBUG_VERBOSE_IRQ

static const struct snd_kcontrol_new dsp_platform_kcontrols[] = {
};

static unsigned int dsp_word_size_align(unsigned int in_size)
{
	unsigned int align_size;

	align_size = in_size & 0xFFFFFF80;
	return align_size;
}

static int afe_remap_dsp_pointer
		(struct buf_attr dst, struct buf_attr src, int bytes)
{
	int retval = bytes;

	retval = (retval * snd_pcm_format_physical_width(src.format))
		  / snd_pcm_format_physical_width(dst.format);
	retval = (retval * src.rate) / dst.rate;
	retval = (retval * src.channel) / dst.channel;
	return retval;
}


static snd_pcm_uframes_t mtk_dsphw_pcm_pointer_ul
			 (struct snd_pcm_substream *substream)
{

	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int id = rtd->cpu_dai->id;
	struct mtk_base_dsp *dsp = snd_soc_platform_get_drvdata(rtd->platform);
	struct mtk_base_dsp_mem *dsp_mem = &dsp->dsp_mem[id];
	int ptr_bytes;

#ifdef DEBUG_VERBOSE
	dump_rbuf_s(__func__, &dsp_mem->ring_buf);
#endif

	ptr_bytes = dsp_mem->ring_buf.pWrite - dsp_mem->ring_buf.pBufBase;

	return bytes_to_frames(substream->runtime, ptr_bytes);
}

static snd_pcm_uframes_t mtk_dsphw_pcm_pointer_dl
			 (struct snd_pcm_substream *substream)
{

	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int id = rtd->cpu_dai->id;
	struct mtk_base_dsp *dsp = snd_soc_platform_get_drvdata(rtd->platform);
	struct mtk_base_dsp_mem *dsp_mem = &dsp->dsp_mem[id];
	struct mtk_base_afe *afe = get_afe_base();
	struct mtk_base_afe_memif *memif =
			&afe->memif[get_afememdl_by_afe_taskid(id)];
	const struct mtk_base_memif_data *memif_data = memif->data;
	struct regmap *regmap = afe->regmap;
	struct device *dev = afe->dev;
	int reg_ofs_base = memif_data->reg_ofs_base;
	int reg_ofs_cur = memif_data->reg_ofs_cur;
	unsigned int hw_ptr = 0, hw_base = 0;
	int ret, pcm_ptr_bytes, pcm_remap_ptr_bytes;
	unsigned long flags;

	ret = regmap_read(regmap, reg_ofs_cur, &hw_ptr);
	if (ret || hw_ptr == 0) {
		dev_err(dev, "1 %s hw_ptr err\n", __func__);
		pcm_ptr_bytes = 0;
		pcm_remap_ptr_bytes = 0;
		goto POINTER_RETURN_FRAMES;
	}

	ret = regmap_read(regmap, reg_ofs_base, &hw_base);
	if (ret || hw_base == 0) {
		dev_err(dev, "2 %s hw_ptr err\n", __func__);
		pcm_ptr_bytes = 0;
		pcm_remap_ptr_bytes = 0;
		goto POINTER_RETURN_FRAMES;
	}

	pcm_ptr_bytes = hw_ptr - hw_base;
	pcm_remap_ptr_bytes =
			afe_remap_dsp_pointer(
			dsp_mem->audio_afepcm_buf.aud_buffer.buffer_attr,
			dsp_mem->adsp_buf.aud_buffer.buffer_attr,
			pcm_ptr_bytes);
	pcm_remap_ptr_bytes = dsp_word_size_align(pcm_remap_ptr_bytes);
	if (pcm_remap_ptr_bytes >=
	    dsp_mem->adsp_buf.aud_buffer.buf_bridge.bufLen)
		pr_info("%s pcm_remap_ptr_bytes = %d",
			__func__,
			pcm_remap_ptr_bytes);
	else
		dsp_mem->adsp_buf.aud_buffer.buf_bridge.pRead =
			(dsp_mem->adsp_buf.aud_buffer.buf_bridge.pBufBase +
			 pcm_remap_ptr_bytes);

	spin_lock_irqsave(&dsp_ringbuf_lock, flags);

#ifdef DEBUG_VERBOSE
	dump_rbuf_bridge_s("1 mtk_dsp_dl_handler",
				&dsp_mem->adsp_buf.aud_buffer.buf_bridge);
	dump_rbuf_s("1 mtk_dsp_dl_handler",
				&dsp_mem->ring_buf);
#endif
	sync_ringbuf_readidx(
		&dsp_mem->ring_buf,
		&dsp_mem->adsp_buf.aud_buffer.buf_bridge);
	spin_unlock_irqrestore(&dsp_ringbuf_lock, flags);

#ifdef DEBUG_VERBOSE
	pr_info("%s id = %d reg_ofs_base = %d reg_ofs_cur = %d pcm_ptr_bytes = %d pcm_remap_ptr_bytes = %d\n",
		 __func__, id, reg_ofs_base, reg_ofs_cur,
		 pcm_ptr_bytes, pcm_remap_ptr_bytes);
#endif

POINTER_RETURN_FRAMES:
	return bytes_to_frames(substream->runtime, pcm_remap_ptr_bytes);
}

static snd_pcm_uframes_t mtk_dsphw_pcm_pointer
			 (struct snd_pcm_substream *substream)
{
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return mtk_dsphw_pcm_pointer_dl(substream);
	else
		return mtk_dsphw_pcm_pointer_ul(substream);

}

static void mtk_dsp_dl_handler(struct mtk_base_dsp *dsp,
			       struct ipi_msg_t *ipi_msg, int id)
{
	if (dsp->dsp_mem[id].substream->runtime->status->state
	    != SNDRV_PCM_STATE_RUNNING) {
		pr_info("%s = state[%d]\n", __func__,
			 dsp->dsp_mem[id].substream->runtime->status->state);
		goto DSP_IRQ_HANDLER_ERR;
	}

	/* notify subsream */
	snd_pcm_period_elapsed(dsp->dsp_mem[id].substream);
DSP_IRQ_HANDLER_ERR:
	return;
}

static void mtk_dsp_ul_handler(struct mtk_base_dsp *dsp,
			       struct ipi_msg_t *ipi_msg, int id)
{
	struct mtk_base_dsp_mem *dsp_mem = &dsp->dsp_mem[id];
	void *ipi_audio_buf;
	unsigned long flags;

	if (dsp->dsp_mem[id].substream->runtime->status->state
	    != SNDRV_PCM_STATE_RUNNING) {
		pr_info("%s = state[%d]\n", __func__,
			 dsp->dsp_mem[id].substream->runtime->status->state);
		goto DSP_IRQ_HANDLER_ERR;
	}

	/* upadte for write index*/
	ipi_audio_buf = (void *)dsp_mem->msg_dtoa_share_buf.va_addr;
	memcpy((void *)&dsp_mem->adsp_work_buf, (void *)ipi_audio_buf,
	       sizeof(struct audio_hw_buffer));

	dsp_mem->adsp_buf.aud_buffer.buf_bridge.pWrite =
		(dsp_mem->adsp_work_buf.aud_buffer.buf_bridge.pWrite);
#ifdef DEBUG_VERBOSE
	dump_rbuf_bridge_s(__func__,
			   &dsp_mem->adsp_work_buf.aud_buffer.buf_bridge);
	dump_rbuf_bridge_s(__func__,
			   &dsp_mem->adsp_buf.aud_buffer.buf_bridge);
#endif

	spin_lock_irqsave(&dsp_ringbuf_lock, flags);
	sync_ringbuf_writeidx(&dsp_mem->ring_buf,
			      &dsp_mem->adsp_buf.aud_buffer.buf_bridge);
	spin_unlock_irqrestore(&dsp_ringbuf_lock, flags);

#ifdef DEBUG_VERBOSE
	dump_rbuf_s(__func__, &dsp_mem->ring_buf);
#endif

	/* notify subsream */
	snd_pcm_period_elapsed(dsp->dsp_mem[id].substream);
DSP_IRQ_HANDLER_ERR:
	return;
}


static void mtk_dsp_pcm_ipi_recv(struct ipi_msg_t *ipi_msg)
{
	int id = get_dspdaiid_by_dspscene(ipi_msg->task_scene);
	struct mtk_base_dsp *dsp =
		(struct mtk_base_dsp *)get_ipi_recv_private();

	if (dsp == NULL) {
		pr_warn("%s dsp == NULL\n", __func__);
		return;
	} else if (ipi_msg == NULL) {
		pr_warn("%s ipi_msg == NULL\n", __func__);
		return;
	}

	switch (ipi_msg->msg_id) {
	case AUDIO_DSP_TASK_IRQDL:
		mtk_dsp_dl_handler(dsp, ipi_msg, id);
		break;
	case AUDIO_DSP_TASK_IRQUL:
		mtk_dsp_ul_handler(dsp, ipi_msg, id);
		break;
	default:
		break;
	}
}


#if 0
static snd_pcm_uframes_t
mtk_dsp_pcm_pointer(struct snd_pcm_substream *substream)
{
	int pcm_ptr_bytes = 0;

	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mtk_base_dsp *dsp = snd_soc_platform_get_drvdata(rtd->platform);
	int id = rtd->cpu_dai->id;

	struct RingBuf *audio_buf = &(dsp->dsp_mem[id].ring_buf);

#ifdef DEBUG_VERBOSE
	dump_rbuf_s("mtk_dsp_pcm_pointer", &dsp->dsp_mem[id].ring_buf);
#endif

	pcm_ptr_bytes = audio_buf->pRead - audio_buf->pBufBase;
	pcm_ptr_bytes = dsp_word_size_align(pcm_ptr_bytes);

	return bytes_to_frames(substream->runtime, pcm_ptr_bytes);
}
#endif

static int pcmenable(int id)
{
	switch (id) {
	case AUDIO_TASK_PRIMARY_ID:
	case AUDIO_TASK_PLAYBACK_ID:
	case AUDIO_TASK_DEEPBUFFER_ID:
	case AUDIO_TASK_VOIP_ID:
	case AUDIO_TASK_CAPTURE_UL1_ID:
		return 0;
	default:
		pr_warn("%s err id = %d\n", __func__, id);
		return -1;
	}
	return -1;
}

static int mtk_dsp_pcm_open(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct mtk_base_dsp *dsp = snd_soc_platform_get_drvdata(rtd->platform);
	int id = rtd->cpu_dai->id;
	int dsp_feature_id = get_featureid_by_dsp_daiid(id);

	if (pcmenable(id) != 0)
		return 0;
	memcpy((void *)(&(runtime->hw)), (void *)dsp->mtk_dsp_hardware,
	       sizeof(struct snd_pcm_hardware));


	ret = mtk_dsp_register_feature(dsp_feature_id);
	if (ret) {
		pr_info("%s register feature fail", __func__);
		return -1;
	}

	/* send to task with open information */
	mtk_scp_ipi_send(get_dspscene_by_dspdaiid(id), AUDIO_IPI_MSG_ONLY,
			 AUDIO_IPI_MSG_NEED_ACK, AUDIO_DSP_TASK_OPEN, 0, 0,
			 NULL);

	dsp->dsp_mem[id].substream = substream;
	return 0;
}

static int mtk_dsp_pcm_close(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mtk_base_dsp *dsp = snd_soc_platform_get_drvdata(rtd->platform);
	int id = rtd->cpu_dai->id;
	int dsp_feature_id = get_featureid_by_dsp_daiid(id);

	pr_info("%s\n", __func__);

	if (pcmenable(id) != 0)
		return 0;

	/* send to task with close information */
	mtk_scp_ipi_send(get_dspscene_by_dspdaiid(id), AUDIO_IPI_MSG_ONLY,
			 AUDIO_IPI_MSG_NEED_ACK, AUDIO_DSP_TASK_CLOSE, 0, 0,
			 NULL);

	mtk_dsp_deregister_feature(dsp_feature_id);

	dsp->dsp_mem[id].substream = NULL;

	return ret;
}

static int mtk_dsp_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mtk_base_dsp *dsp = snd_soc_platform_get_drvdata(rtd->platform);
	int id = rtd->cpu_dai->id;
	void *ipi_audio_buf; /* dsp <-> audio data struct*/
	int ret = 0;
	struct mtk_base_dsp_mem *dsp_memif = &dsp->dsp_mem[id];

	reset_audiobuffer_hw(&dsp->dsp_mem[id].adsp_buf);
	reset_audiobuffer_hw(&dsp->dsp_mem[id].audio_afepcm_buf);
	reset_audiobuffer_hw(&dsp->dsp_mem[id].adsp_work_buf);
	RingBuf_Reset(&dsp->dsp_mem[id].ring_buf);

	dev_info(dsp->dev, "%s id %d\n", __func__, id);

	dsp->request_dram_resource(dsp->dev);

	/* gen pool related */
	dsp->dsp_mem[id].gen_pool_buffer = mtk_get_adsp_dram_gen_pool(id);
	if (dsp->dsp_mem[id].gen_pool_buffer != NULL) {
		pr_debug("gen_pool_avail = %zu poolsize = %zu\n",
			 gen_pool_avail(dsp->dsp_mem[id].gen_pool_buffer),
			 gen_pool_size(dsp->dsp_mem[id].gen_pool_buffer));

		/* if already allocate , free it.*/
		if (substream->dma_buffer.area) {
			ret = mtk_adsp_genpool_free_sharemem_ring
						(&dsp->dsp_mem[id], id);
			if (!ret)
				release_snd_dmabuffer(&substream->dma_buffer);
		}
		if (ret < 0) {
			pr_warn("%s err\n", __func__);
			return -1;
		}

		/* allocate ring buffer wioth share memory */
		ret = mtk_adsp_genpool_allocate_sharemem_ring(
			&dsp->dsp_mem[id], params_buffer_bytes(params), id);

		if (ret < 0) {
			pr_warn("%s err\n", __func__);
			return -1;
		}

		pr_debug("gen_pool_avail = %zu poolsize = %zu\n",
			 gen_pool_avail(dsp->dsp_mem[id].gen_pool_buffer),
			 gen_pool_size(dsp->dsp_mem[id].gen_pool_buffer));
	}

#ifdef DEBUG_VERBOSE
	dump_audio_dsp_dram(&dsp->dsp_mem[id].msg_atod_share_buf);
	dump_audio_dsp_dram(&dsp->dsp_mem[id].msg_dtoa_share_buf);
	dump_audio_dsp_dram(&dsp->dsp_mem[id].dsp_ring_share_buf);
#endif
	ret = dsp_dram_to_snd_dmabuffer(&dsp->dsp_mem[id].dsp_ring_share_buf,
					&substream->dma_buffer);
	if (ret < 0)
		goto error;
	ret = set_audiobuffer_hw(&dsp->dsp_mem[id].adsp_buf,
				 BUFFER_TYPE_SHARE_MEM);
	if (ret < 0)
		goto error;
	ret = set_audiobuffer_memorytype(&dsp->dsp_mem[id].adsp_buf,
						 MEMORY_AUDIO_DRAM);
	if (ret < 0)
		goto error;
	ret = set_audiobuffer_attribute(&dsp->dsp_mem[id].adsp_buf,
					substream,
					params,
					afe_get_pcmdir(substream->stream,
					dsp->dsp_mem[id].adsp_buf));
	if (ret < 0)
		goto error;

	/* send audio_hw_buffer to SCP side */
	ipi_audio_buf = (void *)dsp->dsp_mem[id].msg_atod_share_buf.va_addr;
	memcpy((void *)ipi_audio_buf, (void *)&dsp->dsp_mem[id].adsp_buf,
	       sizeof(struct audio_hw_buffer));

#ifdef DEBUG_VERBOSE
	dump_rbuf_s(__func__, &dsp->dsp_mem[id].ring_buf);
#endif

	/* send to task with hw_param information , buffer and pcm attribute */
	mtk_scp_ipi_send(get_dspscene_by_dspdaiid(id), AUDIO_IPI_PAYLOAD,
			 AUDIO_IPI_MSG_NEED_ACK, AUDIO_DSP_TASK_HWPARAM,
			 sizeof(unsigned int),
			 (unsigned int)
			 dsp_memif->msg_atod_share_buf.phy_addr,
			 (char *)&dsp->dsp_mem[id].msg_atod_share_buf.phy_addr);

	return ret;

error:
	pr_err("%s err\n", __func__);
	return -1;
}

static int mtk_dsp_pcm_hw_free(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mtk_base_dsp *dsp = snd_soc_platform_get_drvdata(rtd->platform);
	int id = rtd->cpu_dai->id;
	struct gen_pool *gen_pool_dsp;

	pr_info("%s\n", __func__);

	gen_pool_dsp = mtk_get_adsp_dram_gen_pool(id);

	/* send to task with free status */
	mtk_scp_ipi_send(get_dspscene_by_dspdaiid(id), AUDIO_IPI_MSG_ONLY,
			 AUDIO_IPI_MSG_NEED_ACK, AUDIO_DSP_TASK_HWFREE, 1, 0,
			 NULL);

	if (gen_pool_dsp != NULL && substream->dma_buffer.area) {
		ret = mtk_adsp_genpool_free_sharemem_ring
				(&dsp->dsp_mem[id], id);
		if (!ret)
			release_snd_dmabuffer(&substream->dma_buffer);
	}

	/* release dsp memory */
	ret = reset_audiobuffer_hw(&dsp->dsp_mem[id].adsp_buf);

	dsp->release_dram_resource(dsp->dev);

	return ret;
}

static int mtk_dsp_pcm_hw_prepare(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mtk_base_dsp *dsp = snd_soc_platform_get_drvdata(rtd->platform);
	int id = rtd->cpu_dai->id;
	void *ipi_audio_buf; /* dsp <-> audio data struct */
	struct mtk_base_dsp_mem *dsp_memif = &dsp->dsp_mem[id];

	clear_audiobuffer_hw(&dsp->dsp_mem[id].adsp_buf);
	RingBuf_Reset(&dsp->dsp_mem[id].ring_buf);

	ret = set_audiobuffer_threshold(&dsp->dsp_mem[id].adsp_buf, substream);
	if (ret < 0)
		pr_warn("%s set_audiobuffer_attribute err\n", __func__);

	/* send audio_hw_buffer to SCP side */
	ipi_audio_buf = (void *)dsp->dsp_mem[id].msg_atod_share_buf.va_addr;
	memcpy((void *)ipi_audio_buf, (void *)&dsp->dsp_mem[id].adsp_buf,
	       sizeof(struct audio_hw_buffer));

	/* send to task with prepare status */
	mtk_scp_ipi_send(get_dspscene_by_dspdaiid(id), AUDIO_IPI_PAYLOAD,
			 AUDIO_IPI_MSG_NEED_ACK, AUDIO_DSP_TASK_PREPARE,
			 sizeof(unsigned int),
			 (unsigned int)
			 dsp_memif->msg_atod_share_buf.phy_addr,
			 (char *)&dsp->dsp_mem[id].msg_atod_share_buf.phy_addr);
	return ret;
}

static int mtk_dsp_start(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int id = rtd->cpu_dai->id;

	ret = mtk_scp_ipi_send(get_dspscene_by_dspdaiid(id), AUDIO_IPI_MSG_ONLY,
			       AUDIO_IPI_MSG_DIRECT_SEND, AUDIO_DSP_TASK_START,
			       1, 0, NULL);
	return ret;
}

static int copy_count;
static int mtk_dsp_stop(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int id = rtd->cpu_dai->id;

	ret = mtk_scp_ipi_send(get_dspscene_by_dspdaiid(id), AUDIO_IPI_MSG_ONLY,
			       AUDIO_IPI_MSG_DIRECT_SEND, AUDIO_DSP_TASK_STOP,
			       1, 0, NULL);
	copy_count = 0;
	return ret;
}

static int mtk_dsp_pcm_hw_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mtk_base_dsp *dsp = snd_soc_platform_get_drvdata(rtd->platform);

	dev_info(dsp->dev, "%s cmd %d id = %d\n",
		 __func__, cmd, rtd->cpu_dai->id);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		return mtk_dsp_start(substream);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		return mtk_dsp_stop(substream);
	}
	return -EINVAL;
}

static int mtk_dsp_pcm_copy_dl(struct snd_pcm_substream *substream,
			       int copy_size,
			       struct mtk_base_dsp_mem *dsp_mem,
			       void __user *buf)
{
	int ret = 0, availsize = 0;
	unsigned long flags = 0;
	void *ipi_audio_buf; /* dsp <-> audio data struct */
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int id = rtd->cpu_dai->id;
	struct RingBuf *ringbuf = &(dsp_mem->ring_buf);
	struct ringbuf_bridge *buf_bridge =
		&(dsp_mem->adsp_buf.aud_buffer.buf_bridge);


#ifdef DEBUG_VERBOSE
	dump_rbuf_s(__func__, &dsp_mem->ring_buf);
	dump_rbuf_bridge_s(__func__,
			   &dsp_mem->adsp_buf.aud_buffer.buf_bridge);
#endif

	Ringbuf_Check(&dsp_mem->ring_buf);
	Ringbuf_Bridge_Check(
		&dsp_mem->adsp_buf.aud_buffer.buf_bridge);

	spin_lock_irqsave(&dsp_ringbuf_lock, flags);
	availsize = RingBuf_getFreeSpace(ringbuf);

	if (availsize >= copy_size) {
		RingBuf_copyFromUserLinear(ringbuf, buf, copy_size);
		RingBuf_Bridge_update_writeptr(buf_bridge, copy_size);
	} else {
		pr_info("%s fail copy_size = %d availsize = %d\n", __func__,
			 copy_size, RingBuf_getFreeSpace(ringbuf));
	}

	/* send audio_hw_buffer to SCP side*/
	ipi_audio_buf = (void *)dsp_mem->msg_atod_share_buf.va_addr;
	memcpy((void *)ipi_audio_buf, (void *)&dsp_mem->adsp_buf,
	       sizeof(struct audio_hw_buffer));
	spin_unlock_irqrestore(&dsp_ringbuf_lock, flags);

	Ringbuf_Check(&dsp_mem->ring_buf);
	Ringbuf_Bridge_Check(
		&dsp_mem->adsp_buf.aud_buffer.buf_bridge);
	dsp_mem->adsp_buf.counter++;

#ifdef DEBUG_VERBOSE
	dump_rbuf_s(__func__, &dsp_mem->ring_buf);
	dump_rbuf_bridge_s(__func__,
			   dsp_mem->adsp_buf.aud_buffer.buf_bridge);
#endif
	ret = mtk_scp_ipi_send(
			get_dspscene_by_dspdaiid(id), AUDIO_IPI_PAYLOAD,
			AUDIO_IPI_MSG_NEED_ACK, AUDIO_DSP_TASK_DLCOPY,
			sizeof(unsigned int),
			(unsigned int)dsp_mem->msg_atod_share_buf.phy_addr,
			(char *)&dsp_mem->msg_atod_share_buf.phy_addr);

	return ret;
}

static int mtk_dsp_pcm_copy_ul(struct snd_pcm_substream *substream,
			       int copy_size,
			       struct mtk_base_dsp_mem *dsp_mem,
			       void __user *buf)
{
	int ret = 0, availsize = 0;
	unsigned long flags = 0;
	void *ipi_audio_buf; /* dsp <-> audio data struct */
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int id = rtd->cpu_dai->id;
	struct RingBuf *ringbuf = &(dsp_mem->ring_buf);

#ifdef DEBUG_VERBOSE
	dump_rbuf_s(__func__, &dsp_mem->ring_buf);
	dump_rbuf_bridge_s(__func__,
			   &dsp_mem->adsp_buf.aud_buffer.buf_bridge);
#endif
	Ringbuf_Check(&dsp_mem->ring_buf);
	Ringbuf_Bridge_Check(
			&dsp_mem->adsp_buf.aud_buffer.buf_bridge);

	spin_lock_irqsave(&dsp_ringbuf_lock, flags);
	availsize = RingBuf_getFreeSpace(ringbuf);

	if (availsize <= copy_size) {
		pr_info("%s fail copy_size = %d availsize = %d\n", __func__,
			copy_size, RingBuf_getFreeSpace(ringbuf));
		spin_unlock_irqrestore(&dsp_ringbuf_lock, flags);
		return -1;
	}

	/* get audio_buffer from ring buffer */
	ringbuf_copyto_user_linear(buf, &dsp_mem->ring_buf, copy_size);
	sync_bridge_ringbuf_readidx(&dsp_mem->adsp_buf.aud_buffer.buf_bridge,
				    &dsp_mem->ring_buf);
	dsp_mem->adsp_buf.counter++;
	spin_unlock_irqrestore(&dsp_ringbuf_lock, flags);

	ipi_audio_buf = (void *)dsp_mem->msg_atod_share_buf.va_addr;
	memcpy((void *)ipi_audio_buf, (void *)&dsp_mem->adsp_buf,
		sizeof(struct audio_hw_buffer));
	ret = mtk_scp_ipi_send(
			get_dspscene_by_dspdaiid(id), AUDIO_IPI_PAYLOAD,
			AUDIO_IPI_MSG_NEED_ACK, AUDIO_DSP_TASK_ULCOPY,
			sizeof(unsigned int),
			(unsigned int)dsp_mem->msg_atod_share_buf.phy_addr,
			(char *)&dsp_mem->msg_atod_share_buf.phy_addr);

#ifdef DEBUG_VERBOSE
	dump_rbuf_bridge_s("1 mtk_dsp_ul_handler",
				&dsp_mem->adsp_buf.aud_buffer.buf_bridge);
	dump_rbuf_s("1 mtk_dsp_ul_handler",
				&dsp_mem->ring_buf);
#endif
	return ret;
}


static int mtk_dsp_pcm_copy(struct snd_pcm_substream *substream, int channel,
			    snd_pcm_uframes_t pos, void __user *buf,
			    snd_pcm_uframes_t count)
{
	struct snd_pcm_runtime *runtimestream = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int id = rtd->cpu_dai->id;
	struct mtk_base_dsp *dsp = snd_soc_platform_get_drvdata(rtd->platform);
	struct mtk_base_dsp_mem *dsp_mem = &dsp->dsp_mem[id];
	int copy_size, ret = 0;

	copy_size = snd_pcm_format_size(runtimestream->format,
					runtimestream->channels) *
					count;

	if (copy_size <= 0) {
		pr_info(
			"error %s channel = %d pos = %lu count = %lu copy_size = %d\n",
			__func__, channel, pos, count, copy_size);
		return -1;
	}

#ifdef DEBUG_VERBOSE
	pr_info(
		"+%s channel = %d pos = %lu count = %lu copy_size = %d\n",
		__func__, channel, pos, count, copy_size);
#endif
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = mtk_dsp_pcm_copy_dl(substream, copy_size, dsp_mem, buf);
	else
		ret = mtk_dsp_pcm_copy_ul(substream, copy_size, dsp_mem, buf);

	return ret;
}

static int mtk_dsp_pcm_new(struct snd_soc_pcm_runtime *rtd)
{
	int ret = 0;
	size_t size;
	int id = 0;
	struct mtk_base_dsp *dsp = snd_soc_platform_get_drvdata(rtd->platform);

	size = dsp->mtk_dsp_hardware->buffer_bytes_max;

	snd_soc_add_platform_controls(rtd->platform,
				      dsp_platform_kcontrols,
				      ARRAY_SIZE(dsp_platform_kcontrols));

	for (id = 0; id < AUDIO_TASK_DAI_NUM; id++)
		ret = audio_task_register_callback(get_dspscene_by_dspdaiid(id),
						   mtk_dsp_pcm_ipi_recv, NULL);

	mtk_init_adsp_audio_share_mem(dsp);
	if (ret < 0)
		return ret;

	return ret;
}

static const struct snd_pcm_ops mtk_dsp_pcm_ops = {
	.open = mtk_dsp_pcm_open,
	.close = mtk_dsp_pcm_close,
	.hw_params = mtk_dsp_pcm_hw_params,
	.hw_free = mtk_dsp_pcm_hw_free,
	.prepare = mtk_dsp_pcm_hw_prepare,
	.trigger = mtk_dsp_pcm_hw_trigger,
	.ioctl = snd_pcm_lib_ioctl,
	.pointer = mtk_dsphw_pcm_pointer,
	.copy = mtk_dsp_pcm_copy,
};

const struct snd_soc_platform_driver mtk_dsp_pcm_platform = {
	.ops = &mtk_dsp_pcm_ops,
	.pcm_new = mtk_dsp_pcm_new,
};
EXPORT_SYMBOL_GPL(mtk_dsp_pcm_platform);

MODULE_DESCRIPTION("Mediatek dsp platform driver");
MODULE_AUTHOR("chipeng Chang <chipeng.chang@mediatek.com>");
MODULE_LICENSE("GPL v2");
