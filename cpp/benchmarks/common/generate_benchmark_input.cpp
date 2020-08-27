#include "generate_benchmark_input.hpp"

#include <cudf/column/column.hpp>
#include <cudf/table/table.hpp>

#include <tests/utilities/column_utilities.hpp>
#include <tests/utilities/column_wrapper.hpp>

#include <rmm/device_buffer.hpp>

#include <future>
#include <memory>
#include <random>
#include <thread>
#include <vector>

/**
 * @brief Helper for pinned host memory
 **/
template <typename T>
using pinned_buffer = std::unique_ptr<T[], decltype(&cudaFreeHost)>;
template <typename T>
auto pinned_alloc = [](size_t count) {
  T* ptr = nullptr;
  CUDA_TRY(cudaMallocHost(&ptr, count * sizeof(T)));
  return ptr;
};

/**
 * @file generate_benchmark_input.hpp
 * @brief Contains functions that generate columns filled with random data.
 *
 * Also includes utilies that generate random tables.
 *
 * The distribution of random data is meant to simulate real-world data. For example, numerical
 * values are generated using a normal distribution with a zero mean. Therefore, different column
 * types are filled using different distributions. The distributions are documented in the
 * functions where they are used.
 *
 * Currently, the data generation is done on the CPU and the data is then copied to the device
 * memory.
 */

/**
 * @brief Mersenne Twister engine with static seed.
 *
 * Produces the same random sequence on each run.
 */
auto deterministic_engine(unsigned seed = 13377331) { return std::mt19937{seed}; }

template <typename T>
std::enable_if_t<cudf::is_fixed_width<T>(), size_t> avg_element_size()
{
  return sizeof(T);
}

template <typename T>
std::enable_if_t<!cudf::is_fixed_width<T>(), size_t> avg_element_size()
{
  CUDF_FAIL("not implemented!");
}

template <>
size_t avg_element_size<cudf::string_view>()
{
  return 4 + 4 + 6;  // offset + length + hardcoded avg len
}

struct avg_element_size_fn {
  template <typename T>
  size_t operator()()
  {
    return avg_element_size<T>();
  }
};

size_t avg_element_bytes(cudf::type_id tid)
{
  return cudf::type_dispatcher(cudf::data_type(tid), avg_element_size_fn{});
}

/**
 * @brief nanosecond count in the unit of @ref T.
 *
 * @tparam T Timestamp type
 */
template <typename T>
constexpr int64_t nanoseconds()
{
  using ratio = std::ratio_divide<typename T::period, typename cudf::timestamp_ns::period>;
  return ratio::num / ratio::den;
}

/**
 * @brief Creates an random timestamp
 *
 * Generates 'recent' timestamps. All timstamps are earlier that June 2020. The period between the
 * timestamps and June 2020 has a geometric distribution. Most timestamps are within a few years
 * before 2020.
 *
 * @return The random timestamp
 * @tparam T Timestamp type
 */
template <typename T, std::enable_if_t<cudf::is_timestamp<T>()>* = nullptr>
T random_value(std::mt19937& engine)
{
  // Timestamp for June 2020
  static constexpr int64_t current_ns    = 1591053936l * nanoseconds<cudf::timestamp_s>();
  static constexpr auto timestamp_spread = 1. / (2 * 365 * 24 * 60 * 60);  // one in two years

  // Generate a number of seconds that is 50% likely to be shorter than two years
  std::geometric_distribution<int64_t> seconds_gen{timestamp_spread};
  // Generate a random value for the nanoseconds within a second
  std::uniform_int_distribution<int64_t> nanoseconds_gen{0, nanoseconds<cudf::timestamp_s>()};

  // Subtract the seconds from the 2020 timestamp to generate a reccent timestamp
  auto const timestamp_ns =
    current_ns - seconds_gen(engine) * nanoseconds<cudf::timestamp_s>() - nanoseconds_gen(engine);
  // Return value in the type's precision
  return T(typename T::duration{timestamp_ns / nanoseconds<T>()});
}

template <typename T, std::enable_if_t<cudf::is_duration<T>()>* = nullptr>
T random_value(std::mt19937& engine)
{
  static constexpr auto duration_spread = 1. / (365 * 24 * 60 * 60);  // one in a year

  // Generate a number of seconds that is 50% likely to be shorter than a year
  std::geometric_distribution<int64_t> seconds_gen{duration_spread};
  // Generate a random value for the nanoseconds within a second
  std::uniform_int_distribution<int64_t> nanoseconds_gen{0, nanoseconds<cudf::timestamp_s>()};

  // Subtract the seconds from the 2020 timestamp to generate a reccent timestamp
  auto const duration_ns =
    seconds_gen(engine) * nanoseconds<cudf::timestamp_s>() + nanoseconds_gen(engine);
  // Return value in the type's precision
  return T(typename T::duration{duration_ns / nanoseconds<T>()});
}

template <typename T, std::enable_if_t<cudf::is_fixed_point<T>()>* = nullptr>
T random_value(std::mt19937& engine)
{
  return T{};
}

/**
 * @brief Standard deviation for the Normal distribution used to generate numeric elements.
 *
 * Deviation depends on the type width; wider types -> larger value range.
 */
template <typename T>
constexpr auto stddev()
{
  return 1l << (sizeof(T) * 4);
}

/**
 * @brief Creates an random numeric value with a normal distribution
 *
 * Zero is always used as the mean for teh distribution. Unsigned types are generated as the
 * absolute value of the normal distribution output.
 * Different standard deviations are used depending on the type size, in order to generate larger
 * range of values for when the types supports it.
 *
 * @return The random number
 * @tparam T Numeric type
 */
template <typename T, std::enable_if_t<cudf::is_numeric<T>()>* = nullptr>
T random_value(std::mt19937& engine)
{
  static constexpr T lower_bound = std::numeric_limits<T>::lowest();
  static constexpr T upper_bound = std::numeric_limits<T>::max();

  // Use the type dependent standard deviation
  std::normal_distribution<> gaussian{0., stddev<T>()};

  auto elem = gaussian(engine);
  // Use absolute value for unsigned types
  if (lower_bound >= 0) elem = abs(elem);
  elem = std::max(std::min(elem, (double)upper_bound), (double)lower_bound);

  return T(elem);
}

/**
 * @brief Creates an boolean value with 50:50 probability
 *
 * @return The random boolean value
 */
template <>
bool random_value<bool>(std::mt19937& engine)
{
  std::uniform_int_distribution<> uniform{0, 1};
  return uniform(engine) == 1;
}

size_t null_mask_size(cudf::size_type num_rows)
{
  auto const bits_per_word = sizeof(cudf::bitmask_type) * 8;
  return (num_rows + bits_per_word - 1) / bits_per_word;
}
bool get_null_mask_bit(std::vector<cudf::bitmask_type> const& null_mask_data, cudf::size_type row)
{
  auto const bits_per_word = sizeof(cudf::bitmask_type) * 8;
  return null_mask_data[row / bits_per_word] & (cudf::bitmask_type(1) << row % bits_per_word);
}

void reset_null_mask_bit(std::vector<cudf::bitmask_type>& null_mask_data, cudf::size_type row)
{
  auto const bits_per_word = sizeof(cudf::bitmask_type) * 8;
  null_mask_data[row / bits_per_word] &= ~(cudf::bitmask_type(1) << row % bits_per_word);
}

template <typename T, typename Val_gen, typename Valid_gen>
void set_element_at(Val_gen const& value_gen,
                    Valid_gen const& valid_gen,
                    T* values,
                    std::vector<cudf::bitmask_type>& null_mask,
                    cudf::size_type idx)
{
  if (valid_gen()) {
    values[idx] = value_gen();
  } else {
    reset_null_mask_bit(null_mask, idx);
  }
}

/**
 * @brief Creates a column with random content of the given type
 *
 * The templated implementation is used for all fixed width types. String columns are generated
 * using the specialization implemented below.
 *
 * @param[in] TODO
 *
 * @return Column filled with random data
 */
template <typename T>
std::unique_ptr<cudf::column> create_random_column(std::mt19937& engine, cudf::size_type num_rows)
{
  float const null_frequency        = 0.01;
  cudf::size_type const cardinality = 1000;
  cudf::size_type const avg_run_len = 4;

  std::uniform_real_distribution<float> null_dist;
  auto value_gen = [&]() { return random_value<T>(engine); };
  auto valid_gen = [&]() { return null_frequency == 0.f || null_dist(engine) >= null_frequency; };

  pinned_buffer<T> samples{pinned_alloc<T>(cardinality), cudaFreeHost};
  std::vector<cudf::bitmask_type> samples_null_mask(null_mask_size(cardinality), ~0);
  for (cudf::size_type si = 0; si < cardinality; ++si) {
    set_element_at(value_gen, valid_gen, samples.get(), samples_null_mask, si);
  }

  std::uniform_int_distribution<cudf::size_type> sample_dist{0, cardinality - 1};
  std::gamma_distribution<float> run_len_dist(4.f, avg_run_len / 4.f);
  pinned_buffer<T> data{pinned_alloc<T>(num_rows), cudaFreeHost};
  std::vector<cudf::bitmask_type> null_mask(null_mask_size(num_rows), ~0);

  for (cudf::size_type row = 0; row < num_rows; ++row) {
    if (cardinality == 0) {
      set_element_at(value_gen, valid_gen, data.get(), null_mask, row);
    } else {
      auto const sample_idx = sample_dist(engine);
      set_element_at([&]() { return samples[sample_idx]; },
                     [&]() { return get_null_mask_bit(samples_null_mask, sample_idx); },
                     data.get(),
                     null_mask,
                     row);
    }

    if (avg_run_len > 1) {
      int const run_len = std::min<int>(num_rows - row, std::round(run_len_dist(engine)));
      for (int offset = 1; offset < run_len; ++offset) {
        set_element_at([&]() { return data[row]; },
                       [&]() { return get_null_mask_bit(null_mask, row); },
                       data.get(),
                       null_mask,
                       row + offset);
      }
      row += std::max(run_len - 1, 0);
    }
  }

  return std::make_unique<cudf::column>(
    cudf::data_type{cudf::type_to_id<T>()},
    num_rows,
    rmm::device_buffer(data.get(), num_rows * sizeof(T), cudaStream_t(0)),
    rmm::device_buffer(
      null_mask.data(), null_mask.size() * sizeof(cudf::bitmask_type), cudaStream_t(0)));
}

struct string_col_data {
  std::vector<char> chars;
  std::vector<int32_t> offsets;
  std::vector<cudf::bitmask_type> null_mask;
  explicit string_col_data(cudf::size_type rows, cudf::size_type size)
  {
    offsets.reserve(rows + 1);
    offsets.push_back(0);
    chars.reserve(size);
    null_mask.insert(null_mask.end(), null_mask_size(rows), ~0);
  }
};

// Assumes that the null mask is initialized with all bits valid
void copy_string(cudf::size_type src_idx,
                 string_col_data const& src,
                 cudf::size_type dst_idx,
                 string_col_data& dst)
{
  if (!get_null_mask_bit(src.null_mask, src_idx)) reset_null_mask_bit(dst.null_mask, dst_idx);
  auto const str_len = src.offsets[src_idx + 1] - src.offsets[src_idx];
  dst.chars.resize(dst.chars.size() + str_len);
  // TODO don't copy if null?
  std::copy_n(
    src.chars.begin() + src.offsets[src_idx], str_len, dst.chars.begin() + dst.offsets.back());
  dst.offsets.push_back(dst.chars.size());
}

template <typename Length_gen, typename Char_gen, typename Valid_gen>
void append_string(Length_gen const& len_gen,
                   Char_gen const& char_gen,
                   Valid_gen const& valid_gen,
                   string_col_data& column_data)
{
  auto const idx = column_data.offsets.size() - 1;
  column_data.offsets.push_back(column_data.offsets.back() + len_gen());
  std::generate_n(std::back_inserter(column_data.chars),
                  column_data.offsets[idx + 1] - column_data.offsets[idx],
                  [&]() { return char_gen(); });

  // TODO: use empty string for invalid fields?
  if (!valid_gen()) { reset_null_mask_bit(column_data.null_mask, idx); }
}

/**
 * @brief Creates a string column with random content
 *
 * Uses a Poisson distribution around the mean string length. The average length of elements is
 * 16 and currently there is no way to modify this via parameters.
 *
 * Due to random generation of the length of the columns elements, the resulting column will
 * have a slightly different size from @ref col_bytes.
 *
 * @param[in] TODO
 *
 * @return Column filled with random data
 */
template <>
std::unique_ptr<cudf::column> create_random_column<cudf::string_view>(std::mt19937& engine,
                                                                      cudf::size_type num_rows)
{
  size_t constexpr bits_per_word = sizeof(cudf::bitmask_type) * 8;

  float const null_frequency        = 0.01;
  int const avg_string_len          = 16;
  cudf::size_type const cardinality = 1000;
  cudf::size_type const avg_run_len = 4;

  auto const char_cnt = avg_string_len * num_rows;

  std::poisson_distribution<> len_dist(avg_string_len);
  std::uniform_real_distribution<float> null_dist;
  std::gamma_distribution<float> run_len_dist(4.f, avg_run_len / 4.f);
  std::uniform_int_distribution<char> char_dist{'!', '~'};
  auto length_gen = [&]() { return len_dist(engine); };
  auto char_gen   = [&]() { return char_dist(engine); };
  auto valid_gen  = [&]() { return null_frequency == 0.f || null_dist(engine) >= null_frequency; };

  string_col_data samples(cardinality, cardinality * avg_string_len);
  for (cudf::size_type si = 0; si < cardinality; ++si) {
    append_string(length_gen, char_gen, valid_gen, samples);
  }

  string_col_data out_col(num_rows, num_rows * avg_string_len);
  std::uniform_int_distribution<cudf::size_type> sample_dist{0, cardinality - 1};
  for (cudf::size_type row = 0; row < num_rows; ++row) {
    if (cardinality == 0) {
      append_string(length_gen, char_gen, valid_gen, out_col);
    } else {
      copy_string(sample_dist(engine), samples, row, out_col);
    }
    if (avg_run_len > 1) {
      int const run_len = std::min<int>(num_rows - row, std::round(run_len_dist(engine)));
      for (int offset = 1; offset < run_len; ++offset) {
        copy_string(row, out_col, row + offset, out_col);
      }
      row += std::max(run_len - 1, 0);
    }
  }

  return cudf::make_strings_column(out_col.chars, out_col.offsets, out_col.null_mask);
}

template <>
std::unique_ptr<cudf::column> create_random_column<cudf::dictionary32>(std::mt19937& engine,
                                                                       cudf::size_type num_rows)
{
  CUDF_FAIL("not implemented yet");
}

template <>
std::unique_ptr<cudf::column> create_random_column<cudf::list_view>(std::mt19937& engine,
                                                                    cudf::size_type num_rows)
{
  CUDF_FAIL("not implemented yet");
}

template <>
std::unique_ptr<cudf::column> create_random_column<cudf::struct_view>(std::mt19937& engine,
                                                                      cudf::size_type num_rows)
{
  CUDF_FAIL("not implemented yet");
}

struct create_rand_col_fn {
 public:
  template <typename T>
  std::unique_ptr<cudf::column> operator()(std::mt19937& engine, cudf::size_type num_rows)
  {
    return create_random_column<T>(engine, num_rows);
  }
};

using columns_vector = std::vector<std::unique_ptr<cudf::column>>;

columns_vector create_random_columns(std::vector<cudf::type_id> dtype_ids,
                                     std::mt19937 engine,
                                     cudf::size_type num_rows)
{
  columns_vector output_columns;
  std::transform(
    dtype_ids.begin(), dtype_ids.end(), std::back_inserter(output_columns), [&](auto tid) {
      return cudf::type_dispatcher(cudf::data_type(tid), create_rand_col_fn{}, engine, num_rows);
    });
  return output_columns;
}

std::vector<cudf::type_id> repeat_dtypes(std::vector<cudf::type_id> const& dtype_ids,
                                         cudf::size_type num_cols)
{
  std::vector<cudf::type_id> out_dtypes;
  out_dtypes.reserve(num_cols);
  for (cudf::size_type col = 0; col < num_cols; ++col)
    out_dtypes.push_back(dtype_ids[col % dtype_ids.size()]);
  return out_dtypes;
}

std::unique_ptr<cudf::table> create_random_table(std::vector<cudf::type_id> dtype_ids,
                                                 cudf::size_type num_cols,
                                                 size_t table_bytes)
{
  auto const out_dtype_ids = repeat_dtypes(dtype_ids, num_cols);
  size_t const avg_row_bytes =
    std::accumulate(out_dtype_ids.begin(), out_dtype_ids.end(), 0ul, [](size_t sum, auto tid) {
      return sum + avg_element_bytes(tid);
    });
  cudf::size_type const num_rows = table_bytes / avg_row_bytes;

  auto const processor_count            = std::thread::hardware_concurrency();
  cudf::size_type const cols_per_thread = (num_cols + processor_count - 1) / processor_count;
  cudf::size_type next_col              = 0;

  auto seed_engine = deterministic_engine();  // pass the seed param here
  std::vector<std::future<columns_vector>> col_futures;
  for (unsigned int i = 0; i < processor_count && next_col < num_cols; ++i) {
    auto thread_engine         = deterministic_engine(random_value<unsigned>(seed_engine));
    auto const thread_num_cols = std::min(num_cols - next_col, cols_per_thread);
    std::vector<cudf::type_id> thread_types(out_dtype_ids.begin() + next_col,
                                            out_dtype_ids.begin() + next_col + thread_num_cols);
    col_futures.emplace_back(std::async(std::launch::async,
                                        create_random_columns,
                                        std::move(thread_types),
                                        std::move(thread_engine),
                                        num_rows));
    next_col += thread_num_cols;
  }

  columns_vector output_columns;
  for (auto& cf : col_futures) {
    auto partial_table = cf.get();
    output_columns.reserve(output_columns.size() + partial_table.size());
    std::move(
      std::begin(partial_table), std::end(partial_table), std::back_inserter(output_columns));
    partial_table.clear();
  }

  return std::make_unique<cudf::table>(std::move(output_columns));
}
