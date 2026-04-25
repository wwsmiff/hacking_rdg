// All pipewire and audio processing code is
// based on https://docs.pipewire.org/tutorial7_8c-example.html

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

#define min(a, b) (a) > (b) ? (b) : (a)
#define max(a, b) (a) > (b) ? (a) : (b)

uint8_t *buffer;

struct data;

struct port {
  struct data *data;
};

struct data {
  struct pw_loop *loop;
  struct pw_filter *filter;
  struct port *in_port_l;
  struct port *in_port_r;
  struct port *out_port_l;
  struct port *out_port_r;

  float vol;
};

static void on_process(void *userdata, struct spa_io_position *position) {
  struct data *data = userdata;
  float *in_l, *in_r, *out_l, *out_r;
  uint32_t n_samples = position->clock.duration;

  in_l = pw_filter_get_dsp_buffer(data->in_port_l, n_samples);
  in_r = pw_filter_get_dsp_buffer(data->in_port_r, n_samples);
  out_l = pw_filter_get_dsp_buffer(data->out_port_l, n_samples);
  out_r = pw_filter_get_dsp_buffer(data->out_port_r, n_samples);

  if ((in_l == NULL || in_r == NULL) || (out_l == NULL || out_r == NULL))
    return;

  // audio processing, for now just change volume.
  for (uint32_t i = 0; i < n_samples; ++i) {
    in_l[i] *= data->vol;
    in_r[i] *= data->vol;
  }

  memcpy(out_l, in_l, n_samples * sizeof(float));
  memcpy(out_r, in_r, n_samples * sizeof(float));
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = on_process,
};

void *audio_processing(void *data) {
  struct pw_loop *loop = (struct pw_loop *)(data);
  while (1) {
    pw_loop_iterate(loop, 0);
  }
}

struct memargs {
  int fd;
  const uint32_t start_addr;
  size_t sz;

  struct data *d;
};

uint8_t *readmem(int fd, const uint32_t start_addr, size_t sz) {
  lseek(fd, start_addr, SEEK_SET);
  ssize_t bytes_read = read(fd, buffer, sz);
  return buffer;
}

void *get_speed(void *data) {
  while (1) {
    struct memargs *args = (struct memargs *)(data);
    float vol =
        (*(float *)readmem(args->fd, args->start_addr, args->sz)) / 120.0f;
    args->d->vol = min(0.9999f, max(0.1f, vol));
  }
}

void printmem(int fd, const uint32_t start_addr, size_t sz) {
  while (true) {
    uint8_t *val = readmem(fd, start_addr, sz);
    printf("\r");
    for (int i = 0; i < 4; ++i) {
      printf("%02x ", buffer[i]);
    }
    printf("\n");
    printf("%f\n", *(float *)buffer);
    usleep(50'000);
    printf("\033[H\033[2J");
  }
}

int main(int argc, char **argv) {
  const uint32_t start_addr = 0x36823db0;
  size_t sz = 4;
  buffer = malloc(sz);
  char path[100];
  sprintf(path, "/proc/%s/mem", argv[1]);
  int proc_fd = open(path, O_RDONLY);

  struct data data = {
      0,
  };

  const struct spa_pod *params[1];
  uint32_t n_params = 0;
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

  pw_init(&argc, &argv);
  data.loop = pw_loop_new(NULL);

  data.filter = pw_filter_new_simple(
      data.loop, "audio-filter",
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                        "Filter", PW_KEY_MEDIA_ROLE, "DSP", NULL),
      &filter_events, &data);

  // inputs (in_l, in_r)
  data.in_port_l = pw_filter_add_port(
      data.filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
      sizeof(struct port) * 2,
      pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                        PW_KEY_PORT_NAME, "input_l", NULL),
      NULL, 0);

  data.in_port_r = pw_filter_add_port(
      data.filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
      sizeof(struct port) * 2,
      pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                        PW_KEY_PORT_NAME, "input_r", NULL),
      NULL, 0);

  // outputs (out_l, out_r)
  data.out_port_l = pw_filter_add_port(
      data.filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
      sizeof(struct port) * 2,
      pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                        PW_KEY_PORT_NAME, "output_l", NULL),
      NULL, 0);

  data.out_port_r = pw_filter_add_port(
      data.filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
      sizeof(struct port) * 2,
      pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                        PW_KEY_PORT_NAME, "output_r", NULL),
      NULL, 0);

  data.vol = 0.1f;

  params[n_params++] = spa_process_latency_build(
      &b, SPA_PARAM_ProcessLatency,
      &SPA_PROCESS_LATENCY_INFO_INIT(.ns = 10 * SPA_NSEC_PER_MSEC));

  if (pw_filter_connect(data.filter, PW_FILTER_FLAG_RT_PROCESS, params,
                        n_params) < 0) {
    fprintf(stderr, "can't connect\n");
    return -1;
  }

  struct memargs args = {
      .fd = proc_fd, .start_addr = start_addr, .sz = sz, .d = &data};

  pthread_t t1 = 0;
  pthread_t t2 = 0;
  pthread_create(&t1, NULL, audio_processing, data.loop);
  pthread_create(&t2, NULL, get_speed, &args);
  pthread_join(t1, NULL);
  pthread_join(t2, NULL);

  pw_filter_destroy(data.filter);
  pw_deinit();

  close(proc_fd);
  return 0;
}
