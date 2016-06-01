#include <linux/cpufreq.h>

static struct cpufreq_frequency_table meson_freq_table[]=
{
    //	0	, CPUFREQ_ENTRY_INVALID    ,
    //	1	, CPUFREQ_ENTRY_INVALID    ,
    {0	, 600000    },
    {1	, 720000   },
    {2	, 816000   },
    {3	, 1008000   },
    {4	, 1200000   },
    {5	, 1320000   },
    {6	, 1488000   },
    {7	, 1536000   },
    {8  , CPUFREQ_TABLE_END},
};

#ifdef CONFIG_FIX_SYSPLL
static struct cpufreq_frequency_table meson_freq_table_fix_syspll[]=
{
    {0	, 600000    },
    {1	, 720000   },
    {2	, 816000   },
    {3	, 1008000   },
    {4	, 1200000   },
    {5	, 1320000   },
    {6	, 1488000   },
    {7	, 1536000   },
    {8  , CPUFREQ_TABLE_END},
};
#endif
