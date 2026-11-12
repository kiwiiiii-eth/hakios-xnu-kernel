/* <linux/clk.h> as ath10k's snoc.c uses it; the layer gives the functions (W4b). */
#ifndef _LX10_LINUX_CLK_H_
#define _LX10_LINUX_CLK_H_
struct clk;
struct device;
struct clk_bulk_data {
	const char *id;
	struct clk *clk;
};
struct clk *devm_clk_get(struct device *dev, const char *id);
int clk_prepare_enable(struct clk *clk);
void clk_disable_unprepare(struct clk *clk);
int devm_clk_bulk_get_optional(struct device *dev, int num_clks, struct clk_bulk_data *clks);
int clk_bulk_prepare_enable(int num_clks, const struct clk_bulk_data *clks);
void clk_bulk_disable_unprepare(int num_clks, const struct clk_bulk_data *clks);
#endif
