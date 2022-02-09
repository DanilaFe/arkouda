#include "ArrowFunctions.h"

/*
  Arrow Error Helpers
  -------------------
  Arrow provides PARQUETASSIGNORTHROW and other similar macros
  to help with error handling, but since we are doing something
  unique (passing back the error message to Chapel to be displayed),
  these helpers are similar to the provided macros but matching our
  functionality. 
*/

// The `ARROWRESULT_OK` macro should be used when trying to
// assign the result of an Arrow/Parquet function to a value that can
// potentially throw an error, so the argument `cmd` is the Arrow
// command to execute and `res` is the desired variable to store the
// result
#define ARROWRESULT_OK(cmd, res)                                \
  {                                                             \
    auto result = cmd;                                          \
    if(!result.ok()) {                                          \
      *errMsg = strdup(result.status().message().c_str());      \
      return ARROWERROR;                                        \
    }                                                           \
    res = result.ValueOrDie();                                  \
  }

// The `ARROWSTATUS_OK` macro should be used when calling an
// Arrow/Parquet function that returns a status. The `cmd`
// argument should be the Arrow function to execute.
#define ARROWSTATUS_OK(cmd)                     \
  if(!check_status_ok(cmd, errMsg))             \
    return ARROWERROR;

bool check_status_ok(arrow::Status status, char** errMsg) {
  if(!status.ok()) {
    *errMsg = strdup(status.message().c_str());
    return false;
  }
  return true;
}

/*
 C++ functions
 -------------
 These C++ functions are used to call into the Arrow library
 and are then called to by their corresponding C functions to
 allow interoperability with Chapel. This means that all of the
 C++ functions must return types that are C compatible.
*/

int64_t cpp_getNumRows(const char* filename, char** errMsg) {
  std::shared_ptr<arrow::io::ReadableFile> infile;
  ARROWRESULT_OK(arrow::io::ReadableFile::Open(filename, arrow::default_memory_pool()),
                 infile);

  std::unique_ptr<parquet::arrow::FileReader> reader;
  ARROWSTATUS_OK(parquet::arrow::OpenFile(infile, arrow::default_memory_pool(), &reader));
  
  return reader -> parquet_reader() -> metadata() -> num_rows();
}

int cpp_getType(const char* filename, const char* colname, char** errMsg) {
  std::shared_ptr<arrow::io::ReadableFile> infile;
  ARROWRESULT_OK(arrow::io::ReadableFile::Open(filename, arrow::default_memory_pool()),
                 infile);

  std::unique_ptr<parquet::arrow::FileReader> reader;
  ARROWSTATUS_OK(parquet::arrow::OpenFile(infile, arrow::default_memory_pool(), &reader));

  std::shared_ptr<arrow::Schema> sc;
  std::shared_ptr<arrow::Schema>* out = &sc;
  ARROWSTATUS_OK(reader->GetSchema(out));

  int idx = sc -> GetFieldIndex(colname);
  // Since this doesn't actually throw a Parquet error, we have to generate
  // our own error message for this case
  if(idx == -1) {
    std::string fname(filename);
    std::string dname(colname);
    std::string msg = "Dataset: " + dname + " does not exist in file: " + filename; 
    *errMsg = strdup(msg.c_str());
    return ARROWERROR;
  }
  auto myType = sc -> field(idx) -> type();

  if(myType->id() == arrow::Type::INT64)
    return ARROWINT64;
  else if(myType->id() == arrow::Type::INT32)
    return ARROWINT32;
    else if(myType->id() == arrow::Type::UINT64)
    return ARROWUINT64;
  else if(myType->id() == arrow::Type::TIMESTAMP)
    return ARROWTIMESTAMP;
  else // TODO: error type not supported
    return ARROWUNDEFINED;
}

int cpp_readColumnByName(const char* filename, void* chpl_arr, const char* colname, int64_t numElems, int64_t batchSize, char** errMsg) {
  int64_t ty = cpp_getType(filename, colname, errMsg);
  
  // Currently only supports int64 and int32 Arrow types
  if(ty == ARROWINT64 || ty == ARROWINT32 || ty == ARROWUINT64) {
    std::unique_ptr<parquet::ParquetFileReader> parquet_reader =
      parquet::ParquetFileReader::OpenFile(filename, false);

    std::shared_ptr<parquet::FileMetaData> file_metadata = parquet_reader->metadata();
    int num_row_groups = file_metadata->num_row_groups();

    int64_t i = 0;
    for (int r = 0; r < num_row_groups; r++) {
      std::shared_ptr<parquet::RowGroupReader> row_group_reader =
        parquet_reader->RowGroup(r);

      int64_t values_read = 0;

      std::shared_ptr<parquet::ColumnReader> column_reader;

      auto idx = file_metadata -> schema() -> ColumnIndex(colname);

      if(idx < 0) {
        std::string dname(colname);
        std::string fname(filename);
        std::string msg = "Dataset: " + dname + " does not exist in file: " + fname; 
        *errMsg = strdup(msg.c_str());
        return ARROWERROR;
      }
      
      column_reader = row_group_reader->Column(idx);

      if(ty == ARROWINT64) {
        auto chpl_ptr = (int64_t*)chpl_arr;
        parquet::Int64Reader* reader =
          static_cast<parquet::Int64Reader*>(column_reader.get());

        while (reader->HasNext()) {
          (void)reader->ReadBatch(batchSize, nullptr, nullptr, &chpl_ptr[i], &values_read);
          i+=values_read;
        }
      } else if(ty == ARROWUINT64) {
        auto chpl_ptr = (int64_t*)chpl_arr;
        parquet::Int64Reader* reader =
          static_cast<parquet::Int64Reader*>(column_reader.get());

        while (reader->HasNext()) {
          (void)reader->ReadBatch(batchSize, nullptr, nullptr, &chpl_ptr[i], &values_read);
          i+=values_read;
        }
      } else {
        auto chpl_ptr = (int64_t*)chpl_arr;
        parquet::Int32Reader* reader =
          static_cast<parquet::Int32Reader*>(column_reader.get());

        int32_t* tmpArr = (int32_t*)malloc(batchSize * sizeof(int32_t));
        while (reader->HasNext()) {
          // Can't read directly into chpl_ptr because it is int64
          (void)reader->ReadBatch(batchSize, nullptr, nullptr, tmpArr, &values_read);
          for (int64_t j = 0; j < values_read; j++)
            chpl_ptr[i+j] = (int64_t)tmpArr[j];
          i+=values_read;
        }
        free(tmpArr);
      }
    }
    return 0;
  }
  return ARROWUNDEFINED;
}

int cpp_writeColumnToParquet(const char* filename, void* chpl_arr,
                             int64_t colnum, const char* dsetname, int64_t numelems,
                             int64_t rowGroupSize, int64_t dtype, char** errMsg) {
  auto chpl_ptr = (int64_t*)chpl_arr;
  using FileClass = ::arrow::io::FileOutputStream;
  std::shared_ptr<FileClass> out_file;
  PARQUET_ASSIGN_OR_THROW(out_file, FileClass::Open(filename));

  // Setup schema of a single int64 column
  parquet::schema::NodeVector fields;
  if(dtype == 1)
    fields.push_back(parquet::schema::PrimitiveNode::Make(dsetname, parquet::Repetition::REQUIRED, parquet::Type::INT64, parquet::ConvertedType::NONE));
  else
    fields.push_back(parquet::schema::PrimitiveNode::Make(dsetname, parquet::Repetition::REQUIRED, parquet::Type::INT64, parquet::ConvertedType::UINT_64));
  std::shared_ptr<parquet::schema::GroupNode> schema = std::static_pointer_cast<parquet::schema::GroupNode>
    (parquet::schema::GroupNode::Make("schema", parquet::Repetition::REQUIRED, fields));

  // TODO: add conditionals and arguments for writing with Snappy/RLE
  parquet::WriterProperties::Builder builder;
  std::shared_ptr<parquet::WriterProperties> props = builder.build();

  std::shared_ptr<parquet::ParquetFileWriter> file_writer =
    parquet::ParquetFileWriter::Open(out_file, schema, props);

  int64_t i = 0;
  int64_t numLeft = numelems;

  while(numLeft > 0) {
    parquet::RowGroupWriter* rg_writer = file_writer->AppendRowGroup();
    parquet::Int64Writer* int64_writer =
      static_cast<parquet::Int64Writer*>(rg_writer->NextColumn());

    int64_t batchSize = rowGroupSize;
    if(numLeft < rowGroupSize)
      batchSize = numLeft;
    int64_writer->WriteBatch(batchSize, nullptr, nullptr, &chpl_ptr[i]);
    numLeft -= batchSize;
    i += batchSize;
  }

  file_writer->Close();
  ARROWSTATUS_OK(out_file->Close());

  return 0;
}

const char* cpp_getVersionInfo(void) {
  return strdup(arrow::GetBuildInfo().version_string.c_str());
}

void cpp_free_string(void* ptr) {
  free(ptr);
}

/*
 C functions
 -----------
 These C functions provide no functionality, since the C++
 Arrow library is being used, they merely call the C++ functions
 to allow Chapel to call the C++ functions through C interoperability.
 Each Arrow function must have a corresponding C function if wished
 to be called by Chapel.
*/

extern "C" {
  int64_t c_getNumRows(const char* chpl_str, char** errMsg) {
    return cpp_getNumRows(chpl_str, errMsg);
  }

  int c_readColumnByName(const char* filename, void* chpl_arr, const char* colname, int64_t numElems, int64_t batchSize, char** errMsg) {
    return cpp_readColumnByName(filename, chpl_arr, colname, numElems, batchSize, errMsg);
  }

  int c_getType(const char* filename, const char* colname, char** errMsg) {
    return cpp_getType(filename, colname, errMsg);
  }

  int c_writeColumnToParquet(const char* filename, void* chpl_arr,
                             int64_t colnum, const char* dsetname, int64_t numelems,
                             int64_t rowGroupSize, int64_t dtype, char** errMsg) {
    return cpp_writeColumnToParquet(filename, chpl_arr, colnum, dsetname,
                                    numelems, rowGroupSize, dtype, errMsg);
  }

  const char* c_getVersionInfo(void) {
    return cpp_getVersionInfo();
  }

  void c_free_string(void* ptr) {
    cpp_free_string(ptr);
  }
}