// Test fixture generator for table-model tsfiles (no Qt GUI involved).
// Writes one table with a STRING tag, INT64/DOUBLE/STRING field columns
// across two "devices" (tag combinations), so the viewer's table path can
// be exercised without the COMAC tree-only archives.

#include "common/tablet.h"
#include "common/tsfile_common.h"
#include "file/write_file.h"
#include "writer/tsfile_table_writer.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define O_BINARY_FLAG O_BINARY
#else
#include <fcntl.h>
#include <sys/stat.h>
#define O_BINARY_FLAG 0
#endif

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: TsFileTableFixture <out.tsfile>\n");
        return 1;
    }
    storage::libtsfile_init();

    std::vector<storage::MeasurementSchema*> measurementSchemas;
    std::vector<common::ColumnCategory> categories;
    measurementSchemas.emplace_back(new storage::MeasurementSchema(
        "device", common::TSDataType::TEXT, common::TSEncoding::PLAIN,
        common::CompressionType::UNCOMPRESSED));
    categories.emplace_back(common::ColumnCategory::TAG);
    measurementSchemas.emplace_back(new storage::MeasurementSchema(
        "s0", common::TSDataType::INT64, common::TSEncoding::PLAIN,
        common::CompressionType::UNCOMPRESSED));
    categories.emplace_back(common::ColumnCategory::FIELD);
    measurementSchemas.emplace_back(new storage::MeasurementSchema(
        "s1", common::TSDataType::DOUBLE, common::TSEncoding::PLAIN,
        common::CompressionType::UNCOMPRESSED));
    categories.emplace_back(common::ColumnCategory::FIELD);
    measurementSchemas.emplace_back(new storage::MeasurementSchema(
        "s_text", common::TSDataType::TEXT, common::TSEncoding::PLAIN,
        common::CompressionType::UNCOMPRESSED));
    categories.emplace_back(common::ColumnCategory::FIELD);

    storage::TableSchema tableSchema("testTable", measurementSchemas, categories);

    const int rowsPerDevice = 100;
    const int deviceNum = 2;
    storage::Tablet tablet(tableSchema.get_table_name(),
                           tableSchema.get_measurement_names(),
                           tableSchema.get_data_types(),
                           tableSchema.get_column_categories(),
                           rowsPerDevice * deviceNum);

    for (int d = 0; d < deviceNum; ++d)
    {
        const std::string tag = "dev_" + std::to_string(d);
        char* literal = new char[tag.size() + 1];
        std::memcpy(literal, tag.c_str(), tag.size() + 1);
        common::String tagStr(literal, static_cast<uint32_t>(tag.size()));
        const std::string textVal = "row_";
        char* textBuf = new char[textVal.size() + 1];
        std::memcpy(textBuf, textVal.c_str(), textVal.size() + 1);
        common::String textStr(textBuf, static_cast<uint32_t>(textVal.size()));
        for (int i = 0; i < rowsPerDevice; ++i)
        {
            const int row = d * rowsPerDevice + i;
            tablet.add_timestamp(row, 1'000'000LL * (row + 1));
            tablet.add_value(row, "device", tagStr);
            tablet.add_value<int64_t>(row, "s0", row);
            tablet.add_value<double>(row, "s1", row * 0.5);
            tablet.add_value(row, "s_text", textStr);
        }
        delete[] literal;
        delete[] textBuf;
    }

    storage::WriteFile writeFile;
    int flags = O_WRONLY | O_CREAT | O_TRUNC | O_BINARY_FLAG;
    writeFile.create(argv[1], flags, 0666);

    storage::TsFileTableWriter writer(&writeFile, &tableSchema);
    int ret = writer.write_table(tablet);
    if (ret != common::E_OK)
    {
        std::fprintf(stderr, "write_table failed: %d\n", ret);
        return 1;
    }
    ret = writer.flush();
    if (ret != common::E_OK)
    {
        std::fprintf(stderr, "flush failed: %d\n", ret);
        return 1;
    }
    ret = writer.close();
    if (ret != common::E_OK)
    {
        std::fprintf(stderr, "close failed: %d\n", ret);
        return 1;
    }
    std::printf("wrote %s (rows=%d, tables=1)\n", argv[1],
                rowsPerDevice * deviceNum);
    storage::libtsfile_destroy();
    return 0;
}
