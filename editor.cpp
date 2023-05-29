/*** includes ***/
#include <iostream>
using namespace std;
#include <termio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fstream>
#include <vector>
#include <time.h>
#include <cstdarg>


/*** defines ***/
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 4
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

// 按键含义
enum editorKey{ //枚举类型
    BACKSPACE=127,
    ARROW_LEFT=1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

// 染色类型
enum editorHighlight{
    HL_NORMAL=0,
    HL_COMMENT, //单行注释
    HL_MLCOMMENT, //多行注释
    HL_KEYWORD1, //关键字
    HL_KEYWORD2,
    HL_STRING, //字符串
    HL_NUMBER, //数字
    HL_MATCH  //搜索匹配的字符串
};

// 颜色类型标志位
#define HL_HIGHLIGHT_NUMBERS (1<<0) //1左移0位还是1
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** data ***/
// 文档类型
struct editorSyntax{
    string filetype;
    vector<string>& filematch;
    vector<string>& keywords;
    string singleline_comment_start;
    string multiline_comment_start;
    string multiline_comment_end;
    int flags;
};

// 存储每行信息
struct erow{
    int idx;
    int size;
    int rsize;
    string chars;
    string render; //包含制表符
    string hl; //高亮文本
    int hl_open_comment;  //多行注释变量
};

// 终端变量
struct editorConfig{
    int cx,cy;
    int rx; //当制表符存在时的光标
    int rowoff; 
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    vector<erow> row;
    int dirty; //文件内容是否发生改变
    string filename;
    string statusmsg;
    time_t statusmsg_time;
    struct editorSyntax* syntax;
    struct termios orig_termios;
} E;


/*** filetypes ***/
// 不同类型文件有不同的染色库
vector<string> C_HL_extensions={".c", ".h", ".cpp"};
// 关键字，区分一般关键字和数据类型
vector<string> C_HL_keywords={
    "switch","if","while","for","break","continue","return","else","struct","union","typedef","static","enum","class","case",
    "int|","long|","double|","float|","char|","unsigned|","signed|","void|"};
vector<editorSyntax> HLDB={
    {
        "c", //c语言
        C_HL_extensions, //文件扩展名类型
        C_HL_keywords, //关键字
        "//", //注释符
        "/*",
        "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS  //两个标志位都有
    },
};

#define HLDB_ENTRIES (HLDB.size())


/*** prototype ***/
// template<typename T>
void editorSetStatusMessage();
template<typename T, typename... Args>
void editorSetStatusMessage(const T& fmt,const Args&... args);
void editorRefreshScreen();
string editorPrompt(string& prompt, void(*callback)(string&,int));


/*** terminal ***/
void die(const char* s) {
    // 退出后清空屏幕
    write(STDOUT_FILENO, "\x1b[2J", 4); //\x1b 是 ASCII 转义字符，[2J 的含义是执行终端的清屏操作，将终端屏幕上的内容清除掉，使得屏幕上只剩下空白。
    write(STDOUT_FILENO, "\x1b[H", 3); //[H 的含义是将光标定位到终端的左上角，即行首和列首的位置。
    string errorMsg(s);
    perror(s);
    throw runtime_error(errorMsg);
}

// 程序退出后恢复终端回显
void disableRawMode(){
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios)==-1){
        die("tcsetattr");
    }
}

// 关闭终端回显，使输入不能在终端中显示
void enableRawMode(){
    if(tcgetattr(STDIN_FILENO, &E.orig_termios)==-1){
        die("tcgetattr");
    } // 获取终端的属性
    atexit(disableRawMode); // 在退出时恢复原来设置
    struct termios raw=E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); //在终端设备中启用或禁用 XON/XOFF 流控制，关闭ctrl+s/q/m
    raw.c_oflag &= ~(OPOST); //关闭终端以原始模式将数据直接发送给设备，而不进行任何输出处理，例如换行符的转换等。
    raw.c_cflag |= (CS8);
    raw.c_lflag &=~(ECHO | ICANON | IEXTEN | ISIG); // 关闭ECHO标志位和规范模式，使按位读取，且不回显，关掉ctrl+c/z/v对程序的控制。
    // c_lflag一个位掩码，用于设置或检查各种终端模式的属性，当 ICANON 标志位被设置时，终端输入将按行缓冲进行处理。
    raw.c_cc[VMIN] = 0; //将VMIN设置为0意味着在读取数据时不等待任何最小字节数，即使没有收到任何输入也立即返回。
    raw.c_cc[VTIME] = 1; //将VTIME设置为1表示在读取数据时等待0.1秒，如果在0.1秒内没有收到输入，则立即返回。
    if(tcsetattr(STDIN_FILENO,TCSAFLUSH, &raw)==-1){
        die("tcgetattr");
    };
    
}

// 读取输入字符
int editorReadKey(){
    int nread;
    char c;
    while((nread!=read(STDIN_FILENO,&c,1))!=1){
        if(nread==-1 && errno != EAGAIN){
            die("read");
        }
    }
    // 匹配上下左右建来移动光标
    if(c=='\x1b'){
        char seq[3];
        if(read(STDIN_FILENO,&seq[0],1)!=1){
            return '\x1b';
        }
        if(read(STDIN_FILENO,&seq[1],1)!=1){
            return '\x1b';
        }
        if(seq[0]=='['){
            if(seq[1]>='0' && seq[1]<='9'){
                if(read(STDIN_FILENO,&seq[2],1)!=1){
                    return '\x1b';
                }
                if(seq[2]=='~'){
                    switch(seq[1]){
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }
            else{
                switch (seq[1]){
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }
        else if(seq[0]=='O'){
            switch(seq[1]){
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    }
    else{
        return c;
    }
}

// 获取光标位置，从而得到窗口大小
int getCursorPosition(int* rows, int* cols){
    char buf[32];
    unsigned int i=0;
    if(write(STDIN_FILENO,"\x1b[6n",4)!=4){
        return -1;
    }
    while(i<sizeof(buf)-1){
        if(read(STDIN_FILENO,&buf[i],1)!=1){
            break;
        }
        if(buf[i]=='R'){
            break;
        }
        i++;
    }
    buf[i]='\0';
    if(buf[0]!='\x1b' || buf[1]!='['){
        return -1;
    }
    if(sscanf(&buf[2], "%d;%d", rows, cols) != 2){
        return -1;
    }
    cout<<"\r\n&buf[1]: "<<&buf[1]<<"\r\n";
    return 0;
}

// 获取终端窗口的大小
int getWindowSize(int* rows, int* cols){
    struct winsize ws;
    if(ioctl(STDIN_FILENO, TIOCGWINSZ, &ws)==-1 || ws.ws_col==0){ //如果ioctl方法不行，采用光标移动的方法
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12){ //实现光标向下移动 999 行，光标向右移动 999 列的效果。
            return -1;
        }
        return getCursorPosition(rows,cols);
    }
    else{
        *cols=ws.ws_col;
        *rows=ws.ws_row;
        return 0;
    }
    
}


/*** syntax highlighting ***/
// 判断是否是分隔符
int is_separator(int c){
    string temp=",.()+-/*=~%<>[]{};";
    return isspace(c) || c=='\0' || temp.find(c)!=-1;
}

// 标记字符串的高亮属性
void editorUpdateSyntax(erow* row){
    row->hl.resize(row->rsize);
    if(E.syntax==nullptr){
        return;
    }

    vector<string>& keywords=E.syntax->keywords;

    string& scs=E.syntax->singleline_comment_start;
    int scs_len=scs.length();

    string& mcs=E.syntax->multiline_comment_start;
    string& mce=E.syntax->multiline_comment_end;
    int mcs_len=mcs.length();
    int mce_len=mce.length();
    
    int prev_sep=1;
    int in_string=0;
    int in_comment=(row->idx>0 && E.row[row->idx-1].hl_open_comment);

    int i=0;
    while(i<row->rsize){
        char c=row->render[i];
        char prev_hl=(i>0)? row->hl[i-1]:HL_NORMAL;
        // 注释行染色
        if(scs_len && !in_string && !in_comment){
            if(!row->render.compare(i,scs_len,scs)){ // 从第i个字符开始比较scs_len个字符
                int j=i;
                while(j<row->rsize){  //注释符号后面的都染色
                    row->hl[j]=HL_COMMENT;
                    j++;
                }
                break;
            }
        }
        // 多行注释染色
        if(mcs_len && mce_len && !in_string){
            if(in_comment){
                row->hl[i]=HL_MLCOMMENT;
                if(!row->render.compare(i,mce_len,mce)){
                    int j=i;
                    while(j<i+mce_len){
                        row->hl[j]=HL_MLCOMMENT;
                        j++;
                    }
                    i+=mce_len;
                    in_comment=0;
                    prev_sep=1;
                    continue;
                }
                else{
                    i++;
                    continue;
                }
            }
            else if(!row->render.compare(i,mcs_len,mcs)){
                int j=i;
                while(j<i+mcs_len){
                    row->hl[j]=HL_COMMENT;
                    j++;
                }
                i+=mcs_len;
                in_comment=1;
                continue;
            }
        }
        // 字符串染色
        if(E.syntax->flags & HL_HIGHLIGHT_STRINGS){
            if(in_string){
                row->hl[i]=HL_STRING;
                // 考虑转义字符
                if(c=='\\' && i+1<row->rsize){
                    row->hl[i+1]=HL_STRING;
                    i+=2;
                    continue;
                }
                if(c==in_string){
                    in_string=0;
                }
                i++;
                prev_sep=1;
                continue;
            }
            else{
                if(c=='"' || c=='\''){ // 单引号/双引号
                    in_string=c;
                    row->hl[i]=HL_STRING;
                    i++;
                    continue;
                }
            }
        }
        // 数字染色
        if(E.syntax->flags & HL_HIGHLIGHT_NUMBERS){ //根据标志位与运算，是否有染色库
            if((isdigit(c) && (prev_sep || prev_hl==HL_NUMBER)) || (c=='.' && prev_hl==HL_NUMBER)){
                row->hl[i]=HL_NUMBER;
                i++;
                prev_sep=0;
                continue;
            }
        }
        // 关键字染色
        if(prev_sep){
            int j;
            for(j=0; j<keywords.size(); j++){
                int klen=keywords[j].length();
                //是否是基本数据类型
                int kw2=(keywords[j][klen-1]=='|'); 
                if(kw2){
                    klen--;
                }
                if(!row->render.compare(i,klen,keywords[j].substr(0,klen)) && is_separator(row->render[i+klen])){
                    int k=i;
                    while(k<i+klen){
                        row->hl[k]=kw2? HL_KEYWORD2:HL_KEYWORD1;
                        k++;
                    }
                    i+=klen;
                    break;
                }
            }
            if(j<keywords.size()){
                prev_sep=0;
                continue;
            }
        }

        // 分隔符
        prev_sep=is_separator(c);
        i++;
    }
    int changed=(row->hl_open_comment!=in_comment);
    row->hl_open_comment=in_comment;
    if(changed && row->idx+1<E.numrows){
        editorUpdateSyntax(&E.row[row->idx+1]);
    }
}

// 给高亮字符上色
int editorSyntaxToColor(int hl){
    switch(hl){
        case HL_COMMENT:
        case HL_MLCOMMENT:
            return 36; //青色
        case HL_KEYWORD1:
            return 33; //黄色
        case HL_KEYWORD2:
            return 32; //绿色
        case HL_STRING:
            return 35; //品红
        case HL_NUMBER:
            return 31; //红色
        case HL_MATCH:
            return 34;
        default:
            return 37;
    }
}

// 匹配文件类型和染色库
void editorSelectSyntaxHighlight(){
    E.syntax=nullptr;
    if(E.filename==""){
        return;
    }
    int ext=E.filename.find('.');
    for(int j=0; j<HLDB_ENTRIES; j++){
        struct editorSyntax* s=&HLDB[j];
        int i=0;
        while(i<s->filematch.size()){
            int is_ext=(s->filematch[i][0]=='.');
            if((ext!=-1 && is_ext==1 && !E.filename.substr(ext).compare(s->filematch[i])) ||  //compare比较字符串
            (is_ext==0 && s->filematch[i].find(E.filename)!=-1)){
                E.syntax=s;

                int filerow;
                for(filerow=0; filerow<E.numrows; filerow++){
                    editorUpdateSyntax(&E.row[filerow]);
                }
                return;
            }
            i++;
        }
    }
}


/*** row operation ***/
// 处理制表符存在时的光标移动
int editorRowCxToRx(erow* row, int cx){
    int rx=0;
    int j;
    for(j=0; j<cx; j++){
        if(row->chars[j]=='\t'){
            rx+=(KILO_TAB_STOP-1)-(rx%KILO_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow* row, int rx){
    int cur_rx=0;
    int cx;
    for(cx=0; cx<row->size; cx++){
        if(row->chars[cx]=='\t'){
            cur_rx+=(KILO_TAB_STOP-1)-(cur_rx%KILO_TAB_STOP);
        }
        cur_rx++;
        if(cur_rx>rx){
            return cx;
        }
    }
    return cx;
}

// 处理制表符
void editorUpdateRow(erow* row){
    int tabs=0;
    int j;
    for(j=0; j<row->size; j++){
        if(row->chars[j]=='\t'){
            tabs++;
        }
    }
    row->render.resize(row->size+tabs*(KILO_TAB_STOP-1)+1);
    int idx=0;
    for(j=0; j<row->size; j++){
        if(row->chars[j]=='\t'){
            row->render[idx++]=' ';
            while(idx%KILO_TAB_STOP!=0){
                row->render[idx++]=' ';
            }
        }
        else{
            row->render[idx++]=row->chars[j];

        }
    }
    row->rsize=idx;
    
    editorUpdateSyntax(row);
}

// 读取文件多行/插入新的一行
void editorInsertRow(int at, string& s, size_t len){
    if(at<0 || at>E.numrows){
        return;
    }
    E.row.insert(E.row.begin()+at,erow());

    for(int j=at+1; j<=E.numrows; j++){
        E.row[j].idx++;
    }
    E.row[at].idx=at;

    E.row.emplace_back(); //增加一个内存空间
    E.row[at].chars=s;
    E.row[at].size=len;

    E.row[at].rsize=0;
    E.row[at].render="";
    E.row[at].hl="";
    E.row[at].hl_open_comment=0;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

// 在开头删除字符时，两行合并，删除一行
void editorFreeRow(erow* row){
    row->render.clear();
    row->chars.clear();
    row->hl.clear();
}

void editorDelRow(int at){
    if(at<0 || at>=E.numrows){
        return;
    }
    editorFreeRow(&E.row[at]);
    E.row.erase(E.row.begin()+at);
    for(int j=at; j<E.numrows-1;j++){
        E.row[j].idx--;
    }
    E.numrows--;
    E.dirty++;
}

// 插入字符
void editorRowInsertChar(erow* row, int at, int c){
    if(at<0 || at >row->size){
        at=row->size;
    }
    row->chars.insert(at,1,c);
    row->size++;
    editorUpdateRow(row);
    E.dirty++;
}

// 合并两行字符串
void editorRowAppendString(erow* row, string& s, size_t len){
    row->chars+=s;
    row->size+=len;
    editorUpdateRow(row);
    E.dirty++;
}

// 删除字符
void editorRowDelChar(erow* row, int at){
    if(at<0 || at>=row->size){
        return;
    }
    row->chars=row->chars.substr(0,at)+row->chars.substr(at+1);//删除中间at位置的字符
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}


/*** editor operation ***/
// 插入字符
void editorInsertChar(int c){
    if(E.cy==E.numrows){
        string temp="";
        editorInsertRow(E.numrows,temp,0);
    }
    editorRowInsertChar(&E.row[E.cy],E.cx,c);
    E.cx++;
    
}

// 插入新的一行
void editorInsertNewline(){
    if(E.cx==0){
        string temp="";
        editorInsertRow(E.cy,temp,0);
    }
    else{
        erow* row=&E.row[E.cy];
        string temp=row->chars.substr(E.cx);
        editorInsertRow(E.cy+1,temp,row->size-E.cx);
        row=&E.row[E.cy];
        row->size=E.cx;
        row->chars=row->chars.substr(0,E.cx);
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx=0;
}

// 删除字符
void editorDelChar(){
    if(E.cy==E.numrows){
        return;
    }
    if(E.cx==0 && E.cy==0){
        return;
    }
    erow* row=&E.row[E.cy];
    if(E.cx>0){
        editorRowDelChar(row,E.cx-1);
        E.cx--;
    }
    else{//删去换行符，合并成一行
        E.cx=E.row[E.cy-1].size;
        editorRowAppendString(&E.row[E.cy-1],row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}


/*** file i/o ***/
// 将读取的每行信息合成为string返回
string editorRowsToString(int* buflen){
    string buf="";
    int j;
    for(j=0; j<E.numrows; j++){
        buf+=E.row[j].chars;
        buf+='\n'; //读取的时候是到换行符终止，没有换行符
    }
    *buflen=buf.length();
    return buf;
}

// 打开文件
void editorOpen(string& filename){
    E.filename=filename;

    editorSelectSyntaxHighlight();

    ifstream file(filename);
    if(!file.is_open()){ //通过检查 fp 是否为 NULL 来判断是否成功打开文件。
        die("fopen");
    }
    string line;
    // 逐行读取文件
    while(getline(file,line)){ //从输入流对象中读取文本行，并将其存储到字符串对象中。它会读取到换行符（默认情况下是 \n）或指定的分隔符字符为止。
        while(!line.empty() && (line.back()=='\n' || line.back()=='\r')){
            line.pop_back();  //去除字符串 line 末尾的换行符（\n）和回车符（\r）。
        }
        editorInsertRow(E.numrows,line,line.length());
    }  
    file.close();
    // 打开文件时，文件无改变
    E.dirty=0;
}

// 保存文件
void editorSave(){
    if(E.filename==""){
        string temp="Save as (ESC to cancel): ";
        E.filename=editorPrompt(temp,nullptr);
        if(E.filename==""){
            {
                E.statusmsg="";
                editorSetStatusMessage("Save aborted");
            }
            return;
        }
        editorSelectSyntaxHighlight();
    }
    int len;
    string buf=editorRowsToString(&len);
    ofstream file(E.filename,ios::out | ios::trunc); //std::ios::out表示以写入模式打开文件，std::ios::trunc表示如果文件存在，则先截断文件内容。
    if(!file){
        buf.clear();
        {
            E.statusmsg="";
            editorSetStatusMessage("Can't save! I/O error");
        }
    }
    file<<buf;
    // 文件保存后无改变
    E.dirty=0;
    {
        E.statusmsg="";
        editorSetStatusMessage(to_string(len)+" bytes written to disk");
    }
    file.close();
    buf.clear();
}


/*** find ***/
// 增量查找，不能查找处于同一排的目标???
void editorFindCallback(string& query, int key){
    static int last_match=-1; //上一次查询结果
    static int direction=1; //向上/下查找
    static int saved_hl_line; //原来颜色
    static string saved_hl="";

    //搜索后恢复颜色 
    if(saved_hl!=""){
        E.row[saved_hl_line].hl=saved_hl;
        saved_hl.clear();
    }
    // 初始化
    if(key=='\r' || key=='\x1b'){
        last_match=-1;
        direction=1;
        return;
    }
    // 向后查找
    else if(key==ARROW_RIGHT || key==ARROW_DOWN){
        direction=1;
    }
    // 向前查找
    else if(key==ARROW_LEFT || key==ARROW_UP){
        direction=-1;
    }
    else{
        last_match=-1;
        direction=1;
    }

    if(last_match==-1){
        direction=1;
    }
    int current=last_match;
    int i;
    for(i=0; i<E.numrows; i++){
        // 以current为起点，向前向后搜索
        current+=direction;
        if(current==-1){
            current=E.numrows-1;
        }
        else if(current==E.numrows){
            current=0;
        }
        erow* row=&E.row[current];
        int match=row->render.find(query);
        if(match!=-1){
            last_match=current;
            E.cy=current;
            E.cx=editorRowRxToCx(row,match);
            E.rowoff=E.numrows;
            
            // 染色
            saved_hl_line=current;
            saved_hl=row->hl;
            int i;
            for(i=match; i<match+query.length();i++){
                row->hl[i]=HL_MATCH;
            }
            break;
        }
    }
}

void editorFind(){
    int saved_cx=E.cx;
    int saved_cy=E.cy;
    int saved_coloff=E.coloff;
    int saved_rowoff=E.rowoff;

    string temp="Search (Use ESC/Arrows/Enter): ";
    string query=editorPrompt(temp,editorFindCallback);
    if(query!=""){
        query.clear();
    }
    else{
        E.cx=saved_cx;
        E.cy=saved_cy;
        E.coloff=saved_coloff;
        E.rowoff=saved_rowoff;
    }
}

/*** append buffer ***/
// 将字符串追加到缓冲区，最后一次性写出，使用动态字符串
string abuf="";

void abAppend(string s){
    abuf+=s;
}

void abFree() {
    abuf.clear();
}


/*** output ***/
// 滚动条定位到光标的位置
void editorScroll(){
    E.rx=0;
    if(E.cy<E.numrows){
        E.rx=editorRowCxToRx(&E.row[E.cy],E.cx);
    }
    if(E.cy<E.rowoff){
        E.rowoff=E.cy;
    }
    if(E.cy>=E.rowoff+E.screenrows){
        E.rowoff=E.cy-E.screenrows+1;
    }
    if(E.rx<E.coloff){
        E.coloff=E.rx;
    }
    if(E.rx>=E.coloff+E.screencols){
        E.coloff=E.rx-E.screencols+1;
    }
}

// 按行打印
void editorDrawRows(){
    int y;
    for(y=0; y<E.screenrows; y++){
        // 滚动条
        int filerow=y+E.rowoff;
        if(filerow>=E.numrows){
            if(E.numrows==0 && y==E.screenrows/3){ //当不是打开文件时，显示欢迎词
                string welcome="Kilo editor -- version ";
                welcome+=KILO_VERSION;
                if(welcome.length()>E.screencols){
                    welcome=welcome.substr(0,E.screencols);
                }
                // 欢迎词居中
                int padding=(E.screencols-welcome.length())/2;
                if (padding){
                    abAppend("~");
                    padding--;
                }
                while(padding--){
                    abAppend(" ");
                }
                abAppend(welcome);
            }
            else{
                abAppend("~");
            }
        }
        else{
            int len=E.row[filerow].rsize-E.coloff;
            if(len<0){
                len=0;
            }
            if(len>E.screencols){
                len=E.screencols;
            }
            string c=E.row[filerow].render;
            string hl=E.row[filerow].hl;
            int current_color=-1; //减少写入操作
            // 染色
            int j=0;
            for(j=0;j<len;j++){
                string temp="";
                // 处理CTRL+控制字符为正常大写字母
                if(iscntrl(c[j])){
                    char sym=(c[j]<=26)? ('@'+c[j]):'?'; // 字母在@64后面
                    temp+="\x1b[7m";
                    temp+=sym;
                    temp+="\x1b[m";
                    if(current_color!=-1){
                        temp+="\x1b["+to_string(current_color)+'m';
                    }
                }
                else if(hl[j]==HL_NORMAL){
                    if(current_color!=-1){
                        temp+="\x1b[39m"; //"\x1b[39m" 表示重置文本颜色为默认颜色。
                        current_color=-1;
                    }
                    temp+=c[j];
                }
                else{
                    int color=editorSyntaxToColor(hl[j]);
                    if(color!=current_color){
                        current_color=color;
                        temp+="\x1b["+to_string(color)+'m';//[31m 是设置颜色的代码。数字 31 对应红色。在设置之后的文本将以红色显示，直到另一个颜色设置或重置的控制字符出现。
                    }
                    temp+=c[j];
                }
                abAppend(temp);
            }
            abAppend("\x1b[39m");
        }
        abAppend("\x1b[K");  //清除每一行
        abAppend("\r\n");
    }
}

// 文件状态栏（反白显示模式）
void editorDrawStatusBar(){
    abAppend("\x1b[7m"); //\x1b[7m，它切换到反白显示的文本模式
    string status;
    string filename=E.filename!=""? E.filename:"[No Name]";
    if(filename.length()>20){
        filename=filename.substr(0,20);
    }
    string ismodified=E.dirty? "(modified)":"";
    status=filename+" - "+to_string(E.numrows)+" lines "+ismodified;
    int len=status.length();
    if(len>E.screencols){
        status=status.substr(0,E.screencols);
        len=E.screencols;
    }
    abAppend(status);

    string ft=E.syntax==nullptr? "no ft":E.syntax->filetype;  //文件类型
    string rstatus=ft+" | "+to_string(E.cy+1)+'/'+to_string(E.numrows);
    int rlen=rstatus.length();
    while(len<E.screencols){
        // 在最右边打印当前行数
        if(E.screencols-len==rlen){
            abAppend(rstatus);
            break;
        }
        else{
            abAppend(" ");
            len++;
        }
    }
    abAppend("\x1b[m"); //\x1b[m，它用于恢复默认的文本显示模式
    abAppend("\r\n");
}

// 消息栏
void editorDrawMessageBar(){
    abAppend("\x1b[K");
    int msglen=E.statusmsg.length();
    if(msglen>E.screencols){
        msglen=E.screencols;
        E.statusmsg=E.statusmsg.substr(0,E.screencols);
    }
    if(msglen && (time(nullptr)-E.statusmsg_time<5)){
        abAppend(E.statusmsg);
    }
}

// 清空屏幕
void editorRefreshScreen(){
    editorScroll();

    abuf="";
    abAppend("\x1b[?25l"); //在刷新屏幕之前隐藏光标，并在刷新完成后立即再次显示它。
    abAppend("\x1b[H"); //[H 的含义是将光标定位到终端的左上角，即行首和列首的位置。

    editorDrawRows();
    editorDrawStatusBar();
    editorDrawMessageBar();

    string buf="\x1b["+to_string(E.cy-E.rowoff+1)+";"+to_string(E.rx-E.coloff+1)+"H";
    abAppend(buf);
    abAppend("\x1b[?25h");
    // 最后一次性输出
    write(STDOUT_FILENO,abuf.c_str(),abuf.length());  //write函数第二个参数只能传递char*
    abFree();
}

// 状态栏信息
void editorSetStatusMessage(){} //递归终止函数：当没有参数时递归为止
template<typename T, typename... Args> //可变模版参数函数
void editorSetStatusMessage(const T& fmt,const Args&... args){
    // E.statusmsg=""; //不能写在这，不然每次都清空，可变参数模板没有意义
    E.statusmsg+=fmt;
    editorSetStatusMessage(args...);
    E.statusmsg_time=time(nullptr);
}


/*** input ***/
// 新对话框：新建文件名，查找字符串
string editorPrompt(string& prompt, void(*callback)(string&,int)){ //void (*)：指针指向的是一个函数
    string buf="";
    size_t buflen=0;
    while(1){
        {
            E.statusmsg="";
            editorSetStatusMessage(prompt, buf);
        }
        editorRefreshScreen();

        int c=editorReadKey();
        if(c==DEL_KEY || c==CTRL_KEY('h') || c==BACKSPACE){
            if(buflen!=0){
                buflen--;
                buf=buf.substr(0,buflen);
            }
        }
        else if(c=='\x1b'){ //esc退出
            {
                E.statusmsg="";
                editorSetStatusMessage("");
            }
            if(callback){
                callback(buf,c);
            }
            buf.clear();
            return buf;
        }
        else if(c=='\r'){ //回车保存
            if(buflen!=0){
                {
                    E.statusmsg="";
                    editorSetStatusMessage("");
                }
                if(callback){
                    callback(buf,c);
                }
                return buf;
            }
        }
        else if(!iscntrl(c) && c<128){
            buf+=c;
            buflen++;
        }
        if(callback){
            callback(buf,c);
        }
    }
}

// 键盘移动光标
void editorMoveCursor(int key){
    // 限制光标范围
    erow* row=E.cy>=E.numrows? nullptr : &E.row[E.cy];
    switch(key){
        case ARROW_LEFT:
            if(E.cx!=0){
                E.cx--;
            }
            // 当处于行首时，向左回到上一行尾部
            else if(E.cy>0){
                E.cy--;
                E.cx=E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if(row && E.cx<row->size){
                E.cx++;
            }
            // 从上一行尾部向右回到下一行首部
            else if(row && E.cx==row->size){
                E.cy++;
                E.cx=0;
            }
            break;
        case ARROW_UP:
            if(E.cy!=0){
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if(E.cy<E.numrows){
                E.cy++;
            }
            break;
    }
    // 使光标从较长行上下移动到较短行时，光标直接对准较短行的最后
    row=E.cy>=E.numrows? nullptr: &E.row[E.cy];
    int rowlen=row? row->size:0;
    if(E.cx>rowlen){
        E.cx=rowlen;
    }
}

// 处理按键
void editorProcessKeypress(){
    static int quit_times=KILO_QUIT_TIMES;

    int c=editorReadKey(); //获取输入字符，int型变量是因为设置了上下左右键为1000以上的数字，与其他字母对应的ascii码分开
    switch (c){
        case '\r':
            editorInsertNewline();
            break;

        case CTRL_KEY('q'):
            // 未保存就退出
            if(E.dirty && quit_times>0){
                {
                    E.statusmsg="";
                    editorSetStatusMessage("WARNING!!! File has unsaved chages. Press Ctrl-Q " +to_string(quit_times)+" more times to quit.");
                }
                quit_times--;
                return;
            }
            // 退出后清空屏幕
            write(STDOUT_FILENO, "\x1b[2J", 4); //\x1b 是 ASCII 转义字符，[2J 的含义是执行终端的清屏操作，将终端屏幕上的内容清除掉，使得屏幕上只剩下空白。
            write(STDOUT_FILENO, "\x1b[H", 3); //[H 的含义是将光标定位到终端的左上角，即行首和列首的位置。
            exit(0);
            break;
        
        case CTRL_KEY('s'):
            editorSave();
            break;
        
        case HOME_KEY: //光标移动到最左边
            E.cx=0;
            break;
        case END_KEY: //光标移动到最右边
            if(E.cy<E.numrows){
                E.cx=E.row[E.cy].size;
            }
            break;
        
        case CTRL_KEY('f'):
            editorFind();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if(c==DEL_KEY){
                editorMoveCursor(ARROW_RIGHT);
            }
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            { //代码块
                if(c==PAGE_UP){
                    E.cy=E.rowoff;
                }
                else if(c==PAGE_DOWN){
                    E.cy=E.rowoff+E.screenrows-1;
                    if(E.cy>E.numrows){
                        E.cy=E.numrows;
                    }
                }
                int times=E.screenrows;
                while(times--){
                    editorMoveCursor(c==PAGE_UP? ARROW_UP:ARROW_DOWN);  //PAGE_UP/DOWN使光标直接移动到最上面/下面
                }
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quit_times=KILO_QUIT_TIMES;
}

/*** init ***/
void initEditor(){
    E.cx=0;
    E.cy=0;
    E.rx=0;
    E.rowoff=0;
    E.coloff=0;
    E.numrows=0;
    E.dirty=0;
    E.filename="";
    E.statusmsg="";
    E.statusmsg_time=0;

    if(getWindowSize(&E.screenrows,&E.screencols)==-1){
        die("getWindowSize");
    }
    //最后一行是文件状态
    E.screenrows-=2;
}

int main(int argc, char* argv[]){ //argv[0] 是程序的名称，而 argv[1] 是第一个命令行参数。
    try{
        enableRawMode();
        initEditor();
        if(argc>=2){
            string filename(argv[1]);
            editorOpen(filename);
        }
        {
            E.statusmsg="";
            editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");
        }
        while(true){
            editorRefreshScreen();
            editorProcessKeypress();
        }
    }
    catch (const std::exception& e) {
        cout<< "Error: " << e.what()<<endl;
    }
}
