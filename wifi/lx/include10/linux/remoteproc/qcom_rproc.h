/* <linux/remoteproc/qcom_rproc.h>: the modem's restart notices (ours come from drivers/modem). */
#ifndef _LX10_LINUX_REMOTEPROC_QCOM_RPROC_H_
#define _LX10_LINUX_REMOTEPROC_QCOM_RPROC_H_
struct notifier_block;
enum qcom_ssr_notify_type { QCOM_SSR_BEFORE_POWERUP, QCOM_SSR_AFTER_POWERUP, QCOM_SSR_BEFORE_SHUTDOWN, QCOM_SSR_AFTER_SHUTDOWN };
struct qcom_ssr_notify_data {
	const char *name;
	bool crashed;
};
void *qcom_register_ssr_notifier(const char *name, struct notifier_block *nb);
int qcom_unregister_ssr_notifier(void *notify, struct notifier_block *nb);
#endif
