/* <linux/pwrseq/consumer.h>: the SDM845 has no power sequencer for its Wi-Fi; snoc.c treats none as none. */
#ifndef _LX10_LINUX_PWRSEQ_CONSUMER_H_
#define _LX10_LINUX_PWRSEQ_CONSUMER_H_
struct pwrseq_desc;
struct device;
struct pwrseq_desc *devm_pwrseq_get(struct device *dev, const char *target);
int pwrseq_power_on(struct pwrseq_desc *desc);
int pwrseq_power_off(struct pwrseq_desc *desc);
#endif
