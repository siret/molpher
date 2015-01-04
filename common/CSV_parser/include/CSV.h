/* 
 * File:   CSV.h
 * Author: martin
 *
 * Created on 17. prosinec 2014, 10:36
 */

#ifndef CSV_H
#define	CSV_H

#include <string>
#include <sstream>
#include <map>
#include <vector>

using namespace std;

namespace CSVparse {

    enum DataType {
        FLOAT_TYPE
        , STRING_TYPE
    };

    class CSV {
    private:
        string filename;
        string separator;
        string emptyValue;
        char stringSeparator;
        char decimalChar;
        bool headerOn;
        bool rowNamesOn;
        unsigned int rowCount;
        unsigned int columnCount;

        vector<string> header;
        vector<string> rowNames;
        map<unsigned int, DataType> columnIdxDataType;
        map<string, unsigned int> columnNameColumnIdx;
        map<string, unsigned int> rowNameRowIdx;

        map<unsigned int, vector<double> > floatData;
        map<unsigned int, vector<string> > stringData;

        void loadData();
        void saveToken(string &token, unsigned int columnIdx, DataType type);

    public:

        CSV(string filename
                , string sep = ";"
                , string emptyValue = "NA"
                , bool header = true
                , bool rowNames = false
                , char stringSeparator = '"'
                , char decimalChar = '.'
                );

        void write(ostream &stream);
        void writeRow(ostream &stream, unsigned int number);
        void writeHeader(ostream &stream);
        unsigned int getRowCount();
        unsigned int getColumnCount();
        vector<string> getColumnNames();
        vector<string> getRowNames();
        unsigned int getColumnIdx(const string &colName);
        unsigned int getRowIdx(const string &rowName);
        const map<unsigned int, DataType>& getTypeMap();
        const vector<string>& getHeader();
        const vector<double>& GetFloatData(unsigned int columnIdx);
        const vector<string>& GetStringData(unsigned int columnIdx);
        const vector<double>& GetFloatData(const string &colName);
        const vector<string>& GetStringData(const string &colName);
        
        string getEmptyValue();
    };


}

#endif	/* CSV_H */

