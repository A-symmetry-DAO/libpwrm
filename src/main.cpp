#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <getopt.h>
#include <unistd.h>
#include <locale.h>
#include <sys/resource.h>
#include <limits.h>
#include <pthread.h>

#include "cpu/cpu.h"
#include "process/process.h"
#include "perf/perf.h"
#include "perf/perf_bundle.h"
#include "lib.h"


#include "devices/device.h"
#include "devices/devfreq.h"
#include "devices/usb.h"
#include "devices/ahci.h"
#include "measurement/measurement.h"
#include "parameters/parameters.h"

#include "tuning/tuning.h"

#include "devlist.h"

#ifdef CUDAToolkit_FOUND
#include "nvidia/nvml_power.h"
#endif

#include "Figlet.hh"
#include "tclap/CmdLine.h"
#include "tclap/UnlabeledValueArg.h"

#define DEBUGFS_MAGIC          0x64626720

#define NR_OPEN_DEF 1024 * 1024

using namespace spdlog;
using namespace TCLAP;


bool debug_enabled = false; 
int debug_learning = 0;
bool is_root = false;

extern "C" {
	static volatile bool end_thread;
	void* measure_background_thread(void *arg)
	{
		int sleep_time = *((int *) arg);
		while (!end_thread) {
			sleep(sleep_time);
			global_sample_power();
		}
		return 0;
	}
}

static void checkroot() {
	int uid;
	uid = getuid();
	if (uid != 0) {
		is_root = false;
		warn("Not running with root privileges...some subsystems and devices may not be accessible.\n");
	}
	else {
		is_root = true;
	}

}

static int get_nr_open(void) {
	int nr_open = NR_OPEN_DEF;
	ifstream file;

	file.open("/proc/sys/fs/nr_open", ios::in);
	if (file) {
		file >> nr_open;
		file.close();
	}
	return nr_open;
}

static void init(int auto_tune)
{
	static char initialized = 0;
	int ret;
	struct statfs st_fs;
	struct rlimit rlmt;

	if (initialized)
		return;

	checkroot();

	rlmt.rlim_cur = rlmt.rlim_max = get_nr_open();
	setrlimit (RLIMIT_NOFILE, &rlmt);

	if (system("/sbin/modprobe cpufreq_stats > /dev/null 2>&1"))
		info(_("modprobe cpufreq_stats failed."));
#if defined(__i386__) || defined(__x86_64__)
	if (system("/sbin/modprobe msr > /dev/null 2>&1"))
		info(_("modprobe msr failed."));
#endif
	statfs("/sys/kernel/debug", &st_fs);

	if (st_fs.f_type != (long) DEBUGFS_MAGIC) {
		if (access("/bin/mount", X_OK) == 0) {
			ret = system("/bin/mount -t debugfs debugfs /sys/kernel/debug > /dev/null 2>&1");
		} else {
			ret = system("mount -t debugfs debugfs /sys/kernel/debug > /dev/null 2>&1");
		}
		if (ret != 0) {
			if (!auto_tune) {
				error(_("Failed to mount debugfs!\n"));
				error(_("exiting...\n"));
				exit(EXIT_FAILURE);
			} else {
				error(_("Failed to mount debugfs!\n"));
				error(_("Should still be able to auto tune...\n"));
			}
		}
	}

	srand(time(NULL));

	if (access("/var/cache/", W_OK) == 0)
		mkdir("/var/cache/powertop", 0600);
	else
		mkdir("/data/local/powertop", 0600);

	enumerate_cpus();
	create_all_devices();
	create_all_devfreq_devices();
	detect_power_meters();

	register_parameter("base power", 100, 0.5);
	register_parameter("cpu-wakeups", 39.5);
	register_parameter("cpu-consumption", 1.56);
	register_parameter("gpu-operations", 0.5576);
	register_parameter("disk-operations-hard", 0.2);
	register_parameter("disk-operations", 0.0);
	register_parameter("xwakes", 0.1);
	
	initialized = 1;
}

void clean_shutdown()
{
	close_results();
	clean_open_devices();
	clear_all_devices();
	clear_all_devfreq();
	clear_all_cpus();

	return;
}

void one_measurement(int seconds, int sample_interval, char *workload)
{
	create_all_usb_devices();
	start_power_measurement();
	devices_start_measurement();
	start_devfreq_measurement();
	start_process_measurement();
	start_cpu_measurement();

	if (workload && workload[0]) {
		pthread_t thread = 0UL;
		end_thread = false;
		if (pthread_create(&thread, NULL, measure_background_thread, &sample_interval))
			fprintf(stderr, "ERROR: workload measurement thread creation failed\n");

		if (system(workload))
			fprintf(stderr, _("Unknown issue running workload!\n"));

		if (thread)
		{
			end_thread = true;
			pthread_join( thread, NULL);
		}
		global_sample_power();
	} else {
		while (seconds > 0)
		{
			sleep(sample_interval > seconds ? seconds : sample_interval);
			seconds -= sample_interval;
			global_sample_power();
		}
	}

	end_cpu_measurement();
	end_process_measurement();
	collect_open_devices();
	end_devfreq_measurement();
	devices_end_measurement();
	end_power_measurement();

	process_cpu_data();
	process_process_data();
	end_process_data();

	global_power();
	compute_bundle();

	store_results(measurement_time);
	end_cpu_data();
}


void get_info(const string subsystem) {
	if (subsystem == "hw") {
		create_all_devices();
		create_all_usb_devices();
		info("\nPrinting hardware devices...");
		if (debug_enabled) {
			for (ulong i = 0; i < all_devices.size(); i++) {
				info("HW device class: {}. HW device name: {}. Human name: {}.", all_devices[i]->class_name(), all_devices[i]->device_name(), all_devices[i]->human_name());
			}
		}
		else {
			for (ulong i = 0; i < all_devices.size(); i++) {
				info("HW device name: {}. Human name: {}.", all_devices[i]->device_name(), all_devices[i]->human_name());
			}
		}
	}
	else if (subsystem == "rapl") {
		enumerate_cpus();
		create_all_devices();
		if (!get_rapl_device_present())
		{
			return;
		}
		get_rapl_info();
	}
	else if (subsystem == "meter") {
		detect_power_meters();
		print_power_meter_info();
	}
	#ifdef CUDAToolkit_FOUND
	else if (subsystem == "nv") {
		print_nv_devices_info();
	}
	#endif
}

void measure(const string* subsystem, const string* devid, int time) {
	//init(0);
	if (*subsystem == "rapl") {
		enumerate_cpus();
		create_all_devices();
		if (!get_rapl_device_present())
		{
			return;
		}
		
		info("Measuring CPU power usage for {} seconds using Intel RAPL interface...", time);
		start_rapl_cpu_measurement();
		sleep(time);
		auto p = end_rapl_cpu_measurement();
		info("Power usage {}.", p);
	}
	else if (*subsystem == "meter")
	{
		detect_power_meters();
		info("Measuring system power usage for {} seconds using power meter...", time);
		start_power_measurement();
		sleep(time);
		end_power_measurement();
		global_sample_power();
		info("Energy usage for {}s: {}J.", time, global_joules());
	}
	#ifdef CUDAToolkit_FOUND
	else if (*subsystem == "nv") {
		init_nvml();
		unsigned int r = -1;
		measure_nv_device_power(0, 0, &r);
		shutdown_nvml();
		info("Power draw: {:03.2f}W.", r / 1000.0);
	}
	#endif

}

int main(int argc, char *argv[])
{
	setlocale (LC_ALL, "");
	Figlet::small.print("pwrmd");
	try
	{
		CmdLine cmdline("pwrm is a program for measuring and reporting power usage by hardware devices in .", ' ', "0.1", true);
		vector<string> _cmds {"measure", "info"};
		ValuesConstraint<string> cmds(_cmds);
		UnlabeledValueArg<string> cmd("cmd", "The command to run.    \
		\nmeasure - Measure power consumption for the particular subsystem or device.     \
		\ninfo - Print out information for the specified subsystem or device.",  true, "measure", &cmds, cmdline, false);
		#ifdef CUDAToolkit_FOUND
		vector<string> _systems {"hw", "rapl", "cpu", "meter", "nv"};
		#else
		vector<string> _systems {"hw", "rapl", "cpu", "meter"};
		#endif
		ValuesConstraint<string> systems(_systems);
		UnlabeledValueArg<string> subsystem("subsystem", "The subsystem or device to measure or report on.     \
		\nhw - Hardware devices detected by the operating system.                                           \
		\nrapl - Intel Running Average Power Limit CPU hardware interface.     \
		\nmeter - ACPI or other power meters.", true, "hw", &systems, cmdline, false);
		ValueArg<int> time_arg("t", "time","Time in seconds to measure power usage. Default is 10 seconds.",false, 100, "integer", cmdline);
		ValueArg<string> devid_arg("", "devid","The device id, if any. Default is 0.",false, "0", "integer", cmdline);
		SwitchArg debug_arg("d","debug","Enable debug logging.", cmdline, false);
		
		cmdline.parse(argc, argv);

		debug_enabled = debug_arg.getValue();
		if (debug_enabled)
		{
			spdlog::set_level(spdlog::level::debug);
			info("Debug mode enabled.");
		}
		if (cmd.getValue() == "info") {
			get_info(subsystem.getValue());
		}
		else if (cmd.getValue() == "measure") {
			measure(&subsystem.getValue(), &devid_arg.getValue(), time_arg.getValue());
		}

		clean_shutdown();

		exit(EXIT_SUCCESS);
	}
	catch (ArgException &e) 
    { 
    	error("Error parsing option {0}: {1}.", e.argId(), e.error());
    	return 1;
    }
    catch (std::exception &e) 
    { 
   		error("Runtime error parsing options: {0}", e.what());
    	return 1; 
    }
	return EXIT_SUCCESS;
}
