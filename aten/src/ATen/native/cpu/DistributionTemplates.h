#pragma once

#include <ATen/CPUApplyUtils.h>
#include <ATen/Dispatch.h>
#include <ATen/ExpandBase.h>
#include <ATen/core/DistributionsHelper.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/cpu/Loops.h>
#include <limits>
#include <mutex>

namespace at {
namespace native {
namespace templates {
namespace cpu {
namespace {

// ==================================================== Random ========================================================

template<typename RNG>
void random_from_to_kernel(TensorIteratorBase& iter, uint64_t range, int64_t base, RNG generator) {
  AT_DISPATCH_ALL_TYPES_AND3(at::ScalarType::Bool, at::ScalarType::Half, at::ScalarType::BFloat16, iter.dtype(), "random_from_to_kernel_cpu", [&] {
    std::lock_guard<std::mutex> lock(generator->mutex_);
    cpu_serial_kernel(iter, [range, base, generator]() -> scalar_t {
      uniform_int_from_to_distribution<scalar_t> random(range, base);
      return random(generator);
    });
  });
}

// This is the special kernel to handle single specific case:
// from(inclusive) = std::numeric_limits<int64_t>::lowest()
// to(exclusive) = None (= std::numeric_limits<int64_t>::max() + 1)
template<typename RNG>
void random_full_64_bits_range_kernel(TensorIteratorBase& iter, RNG generator) {
  AT_DISPATCH_ALL_TYPES_AND(at::ScalarType::BFloat16, iter.dtype(), "random_full_64_bits_range_kernel_cpu", [&] {
    if constexpr (std::is_same<scalar_t, int64_t>::value ||
        std::is_same<scalar_t, double>::value ||
        std::is_same<scalar_t, float>::value ||
        std::is_same<scalar_t, at::BFloat16>::value) {
      std::lock_guard<std::mutex> lock(generator->mutex_);
      cpu_serial_kernel(iter, [generator]() -> scalar_t {
        uniform_int_full_range_distribution<scalar_t> random;
        return random(generator);
      });
    } else {
      TORCH_CHECK(false, "random_full_64_bits_range_kernel_cpu handles only int64, double, float and bfloat16");
    }
  });
}

template<typename RNG>
struct RandomFromToKernel {
  void operator()(TensorIteratorBase& iter, uint64_t range, int64_t base, c10::optional<Generator> gen) {
    random_from_to_kernel(iter, range, base, check_generator<RNG>(gen));
  }
  void operator()(TensorIteratorBase& iter, c10::optional<Generator> gen) {
    random_full_64_bits_range_kernel(iter, check_generator<RNG>(gen));
  }
};

template<typename RNG>
void random_kernel(TensorIteratorBase& iter, RNG generator) {
  std::lock_guard<std::mutex> lock(generator->mutex_);
  AT_DISPATCH_ALL_TYPES_AND3(at::ScalarType::Half, at::ScalarType::BFloat16, at::ScalarType::Bool, iter.dtype(), "random_kernel_cpu", [&] {
    cpu_serial_kernel(iter, [generator]() -> scalar_t {
      uniform_int_distribution<scalar_t> random;
      return random(generator);
    });
  });
}

template<typename RNG>
struct RandomKernel {
  void operator()(TensorIteratorBase& iter, c10::optional<Generator> gen) {
    random_kernel(iter, check_generator<RNG>(gen));
  }
};

// ==================================================== Normal ========================================================

template <typename scalar_t>
static void normal_fill_16(scalar_t *data, const scalar_t mean, const scalar_t std) {
  for (const auto j : c10::irange(8)) {
    const scalar_t u1 = 1 - data[j]; // [0, 1) -> (0, 1] for log.
    const scalar_t u2 = data[j + 8];
    const scalar_t radius = std::sqrt(-2 * std::log(u1));
    const scalar_t theta = 2.0f * c10::pi<double> * u2;
    data[j] = radius * std::cos(theta) * std + mean;
    data[j + 8] = radius * std::sin(theta) * std + mean;
  }
}

static void normal_fill_16_vectorize(float *data,const Vectorized<float> &two_pi,const Vectorized<float> &one,const Vectorized<float> &minus_two,const Vectorized<float> &mean,const Vectorized<float> &std) {
  using Vec = Vectorized<float>;
  Vec u1=one-Vec::loadu(data);
  Vec u2=Vec::loadu(data+Vec::size());
  Vec radius=(minus_two * u1.log());
  radius=radius.sqrt();
  Vec theta=two_pi * u2;
  Vec output_vec=radius * theta.cos() * std + mean;
  Vec output_vec2=radius * theta.sin() * std + mean;
  output_vec.store(data);
  output_vec2.store(data+Vec::size());
}

template <typename scalar_t, typename RNG>
void normal_fill_vectorize(const TensorBase &self, const scalar_t mean, const scalar_t std, RNG generator) {
  float *data = self.data_ptr<float>();
  auto size = self.numel();
  std::lock_guard<std::mutex> lock(generator->mutex_);
  for (const auto i : c10::irange(size)) {
    at::uniform_real_distribution<scalar_t> uniform(0, 1);
    data[i] = uniform(generator);
  }

  using Vec = Vectorized<float>;
  const Vec two_pi = Vec(2.0f * c10::pi<double>);
  const Vec one = Vec(1.0f);
  const Vec minus_two = Vec(-2.0f);
  const Vec var_vec  = Vec(std);
  const Vec mean_vec = Vec(mean);

  for (int64_t i = 0; i < size - 15; i += 16) {
    if(Vec::size()==8) {
      normal_fill_16_vectorize(data + i, two_pi, one, minus_two, mean_vec, var_vec);
    }
    else{
      normal_fill_16<scalar_t>(data + i, mean, std);
    }
  }
  if (size % 16 != 0) {
    // Recompute the last 16 values.
    data = data + size - 16;
    for (const auto i : c10::irange(16)) {
      at::uniform_real_distribution<scalar_t> uniform(0, 1);
      data[i] = uniform(generator);
    }
    if(Vec::size()==8){
      normal_fill_16_vectorize(data, two_pi, one, minus_two, mean_vec, var_vec);
    }
    else{
      normal_fill_16<scalar_t>(data, mean, std);
    }
  }
}

template <typename scalar_t, typename RNG>
void normal_fill(const TensorBase &self, const scalar_t mean, const scalar_t std, RNG generator) {
  scalar_t *data = self.data_ptr<scalar_t>();
  auto size = self.numel();
  std::lock_guard<std::mutex> lock(generator->mutex_);
  for (const auto i : c10::irange(size)) {
    at::uniform_real_distribution<scalar_t> uniform(0, 1);
    data[i] = uniform(generator);
  }

  for (int64_t i = 0; i < size - 15; i += 16) {
    normal_fill_16<scalar_t>(data + i, mean, std);
  }
  if (size % 16 != 0) {
    // Recompute the last 16 values.
    data = data + size - 16;
    for (const auto i : c10::irange(16)) {
      at::uniform_real_distribution<scalar_t> uniform(0, 1);
      data[i] = uniform(generator);
    }
    normal_fill_16<scalar_t>(data, mean, std);
  }
}

template<typename RNG>
void normal_kernel(const TensorBase &self, double mean, double std, RNG generator) {
  auto size = self.numel();
  if (self.scalar_type() == ScalarType::Float && size >= 16 && self.is_contiguous()) {
    normal_fill_vectorize(self, static_cast<float>(mean), static_cast<float>(std), generator);
  } else {
    AT_DISPATCH_FLOATING_TYPES_AND2(kHalf, kBFloat16, self.scalar_type(), "normal_kernel_cpu", [&] {
      if (size >= 16 && self.is_contiguous()) {
        normal_fill<scalar_t>(self, static_cast<scalar_t>(mean), static_cast<scalar_t>(std), generator);
      } else {
        auto iter = TensorIterator::borrowing_nullary_op(self);
        std::lock_guard<std::mutex> lock(generator->mutex_);
        cpu_serial_kernel(iter, [mean, std, generator]() -> scalar_t {
          at::normal_distribution<double> normal(mean, std);
          return static_cast<scalar_t>(normal(generator));
        });
      }
    });
  }
}

template<typename RNG>
struct NormalKernel {
  void operator()(Tensor& self, double mean, double std, c10::optional<Generator> gen) {
    normal_kernel(self, mean, std, check_generator<RNG>(gen));
  }
};

// ==================================================== Uniform =======================================================

template<typename RNG>
void uniform_kernel(TensorIteratorBase& iter, double from_, double to_, RNG generator) {
  AT_DISPATCH_FLOATING_TYPES_AND2(kHalf, kBFloat16, iter.dtype(), "uniform_kernel_cpu", [&]() {
    std::lock_guard<std::mutex> lock(generator->mutex_);
    auto from = static_cast<scalar_t>(from_);
    auto to = static_cast<scalar_t>(to_);
    at::uniform_real_distribution<scalar_t> uniform(from, to);
    cpu_serial_kernel(iter, [&uniform, generator]() -> scalar_t {
      return static_cast<scalar_t>(uniform(generator));
    });
  });
}

template<typename RNG>
struct UniformKernel {
  void operator()(TensorIteratorBase& iter, double from, double to, c10::optional<Generator> gen) {
    uniform_kernel(iter, from, to, check_generator<RNG>(gen));
  }
};

// ==================================================== Cauchy ========================================================

template<typename RNG>
void cauchy_kernel(TensorIteratorBase& iter, double median, double sigma, RNG generator) {
  AT_DISPATCH_FLOATING_TYPES_AND2(kHalf, kBFloat16, iter.dtype(), "cauchy_cpu", [&]() {
    std::lock_guard<std::mutex> lock(generator->mutex_);
    at::cauchy_distribution<double> cauchy(median, sigma);
    cpu_serial_kernel(iter, [&cauchy, generator]() -> scalar_t {
      return static_cast<scalar_t>(cauchy(generator));
    });
  });
}

template<typename RNG>
struct CauchyKernel {
  void operator()(TensorIteratorBase& iter, double median, double sigma, c10::optional<Generator> gen) {
    cauchy_kernel(iter, median, sigma, check_generator<RNG>(gen));
  }
};

// ================================================== LogNormal =======================================================

template<typename RNG>
void log_normal_kernel(TensorIteratorBase& iter, double mean, double std, RNG generator) {
  AT_DISPATCH_FLOATING_TYPES_AND2(at::ScalarType::Half, at::ScalarType::BFloat16, iter.dtype(), "log_normal_cpu", [&]() {
    std::lock_guard<std::mutex> lock(generator->mutex_);
    at::lognormal_distribution<double> logNormal(mean, std);
    cpu_serial_kernel(iter, [&logNormal, generator]() -> scalar_t {
      return static_cast<scalar_t>(logNormal(generator));
    });
  });
}

template<typename RNG>
struct LogNormalKernel {
  void operator()(TensorIteratorBase& iter, double mean, double std, c10::optional<Generator> gen) {
    log_normal_kernel(iter, mean, std, check_generator<RNG>(gen));
  }
};

// =================================================== Geometric ======================================================

template<typename RNG>
void geometric_kernel(TensorIteratorBase& iter, double p, RNG generator) {
  AT_DISPATCH_ALL_TYPES_AND2(at::ScalarType::Half, at::ScalarType::BFloat16, iter.dtype(), "geometric_cpu", [&]() {
    std::lock_guard<std::mutex> lock(generator->mutex_);
    at::geometric_distribution<double> geometric(p);
    cpu_serial_kernel(iter, [&geometric, generator]() -> scalar_t {
      return static_cast<scalar_t>(geometric(generator));
    });
  });
}

template<typename RNG>
struct GeometricKernel {
  void operator()(TensorIteratorBase& iter, double p, c10::optional<Generator> gen) {
    geometric_kernel(iter, p, check_generator<RNG>(gen));
  }
};

// ================================================== Exponential =====================================================

template<typename RNG>
void exponential_kernel(TensorIteratorBase& iter, double lambda, RNG generator) {
  TORCH_CHECK(isFloatingType(iter.dtype()), "Exponential distribution is a continuous probability distribution. dtype must be a floating point but you specified ", iter.dtype());
  AT_DISPATCH_FLOATING_TYPES_AND2(at::ScalarType::Half, at::ScalarType::BFloat16, iter.dtype(), "exponential_cpu", [&]() {
    std::lock_guard<std::mutex> lock(generator->mutex_);
    at::exponential_distribution<double> exponential(lambda);
    cpu_serial_kernel(iter, [&exponential, generator]() -> scalar_t {
      return static_cast<scalar_t>(exponential(generator));
    });
  });
}

template<typename RNG>
struct ExponentialKernel {
  void operator()(TensorIteratorBase& iter, double lambda, c10::optional<Generator> gen) {
    exponential_kernel(iter, lambda, check_generator<RNG>(gen));
  }
};

// ================================================== Bernoulli =======================================================

template<typename RNG>
void bernoulli_kernel(const TensorBase &self, const TensorBase &p_, RNG generator) {
  AT_DISPATCH_ALL_TYPES_AND3(at::ScalarType::Bool, at::ScalarType::BFloat16, at::ScalarType::Half,
  self.scalar_type(), "bernoulli_tensor_cpu_self_", [&] {
    // See Note [Acquire lock when using random generators]
    std::lock_guard<std::mutex> lock(generator->mutex_);
    using self_t = scalar_t;
    auto p_cpu = p_.to(kCPU);
    auto p = expand_inplace(self, p_cpu);
    auto iter = TensorIteratorConfig()
        .add_output(self)
        .add_input(*p)
        .check_all_same_dtype(false)
        .build();
    if (p->scalar_type() == kDouble) {
      cpu_serial_kernel(iter, [&](const double p_val) -> self_t {
        at::bernoulli_distribution<double> bernoulli(p_val);
        return static_cast<self_t>(bernoulli(generator));
      });
    } else {
      AT_DISPATCH_FLOATING_TYPES_AND2(at::ScalarType::BFloat16, at::ScalarType::Half,
      p->scalar_type(), "bernoulli_tensor_cpu_p_", [&] {
        using p_t = scalar_t;
        cpu_serial_kernel(iter, [&](const p_t p_val) -> self_t {
          at::bernoulli_distribution<float> bernoulli(p_val);
          return static_cast<self_t>(bernoulli(generator));
        });
      });
    }
  });
}

template<typename RNG>
void bernoulli_kernel(const TensorBase &self, double p, RNG generator) {
  AT_DISPATCH_ALL_TYPES_AND3(at::ScalarType::Bool, at::ScalarType::BFloat16, at::ScalarType::Half,
  self.scalar_type(), "bernoulli_scalar_cpu_", [&] {
    // See Note [Acquire lock when using random generators]
    std::lock_guard<std::mutex> lock(generator->mutex_);
    auto iter = TensorIterator::borrowing_nullary_op(self);
    cpu_serial_kernel(iter, [p, generator]() -> scalar_t {
      at::bernoulli_distribution<double> bernoulli(p);
      return static_cast<scalar_t>(bernoulli(generator));
    });
  });
}

template<typename RNG>
struct BernoulliKernel {
  void operator()(const TensorBase &self, double p, c10::optional<Generator> gen) {
    bernoulli_kernel(self, p, check_generator<RNG>(gen));
  }
  void operator()(const TensorBase &self, const TensorBase &p_, c10::optional<Generator> gen) {
    bernoulli_kernel(self, p_, check_generator<RNG>(gen));
  }
};

}}}}}
