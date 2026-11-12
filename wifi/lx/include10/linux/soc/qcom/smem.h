/* <linux/soc/qcom/smem.h> as ath10k's qmi.c uses it (the firmware's version into SMEM's image table) */
#ifndef _LX10_SMEM_H_
#define _LX10_SMEM_H_
#define QCOM_SMEM_HOST_ANY      -1
void *qcom_smem_get(unsigned host, unsigned item, size_t *size);
#endif
