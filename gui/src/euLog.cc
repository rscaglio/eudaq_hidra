#include <QApplication>
#include <QMessageBox>
#include "eudaq/FileNamer.hh"
#include "euLog.hh"
#include "Colours.hh"

#include <cctype>
#include <cstdio>
#include <ctime>


namespace{
  auto dummy0 = eudaq::Factory<eudaq::LogCollector>::
    Register<LogCollectorGUI, const std::string&, const std::string&>(eudaq::cstr2hash("LogCollectorGUI"));
  auto dummy1 = eudaq::Factory<eudaq::LogCollector>::
    Register<LogCollectorGUI, const std::string&, const std::string&>(eudaq::cstr2hash("GuiLogCollector"));
}

LogItemDelegate::LogItemDelegate(LogCollectorModel *model) : m_model(model) {}

void LogItemDelegate::paint(QPainter *painter,
                            const QStyleOptionViewItem &option,
                            const QModelIndex &index) const {
  int level = m_model->GetLevel(index);
  painter->fillRect(option.rect, QBrush(level_colours[level]));
  QItemDelegate::paint(painter, option, index);
}

LogCollectorGUI::LogCollectorGUI(const std::string &name,
				 const std::string &runcontrol)
  : QMainWindow(0, Qt::Widget),
    eudaq::LogCollector(name, runcontrol), m_delegate(&m_model) {
  setupUi(this);
  std::string filename;
  setWindowIcon(QIcon(":/euLog.ico"));
  viewLog->setModel(&m_model);
  viewLog->setItemDelegate(&m_delegate);
  for (int i = 0; i < LogMessage::NumColumns(); ++i) {
    int w = LogMessage::ColumnWidth(i);
    if (w >= 0)
      viewLog->setColumnWidth(i, w);
  }
  int level = 0;
  for (;;) {
    std::string text = eudaq::Status::Level2String(level);
    if (text == "")
      break;
    text = eudaq::to_string(level) + "-" + text;
    cmbLevel->addItem(text.c_str());
    level++;
  }
  cmbLevel->setCurrentIndex(0);//loglevel
  QRect geom(-1,-1, 100, 100);
  QRect geom_from_last_program_run;
  QSettings settings("EUDAQ collaboration", "EUDAQ");

  settings.beginGroup("MainWindowEuLog");
  geom_from_last_program_run.setSize(settings.value("size", geom.size()).toSize());
  geom_from_last_program_run.moveTo(settings.value("pos", geom.topLeft()).toPoint());
  settings.endGroup();

  QSize fsize = frameGeometry().size();
  if ((geom.x() == -1)||(geom.y() == -1)||(geom.width() == -1)||(geom.height() == -1)) {
    if ((geom_from_last_program_run.x() == -1)||(geom_from_last_program_run.y() == -1)||(geom_from_last_program_run.width() == -1)||(geom_from_last_program_run.height() == -1)) {
      geom.setX(x()); 
      geom.setY(y());
      geom.setWidth(fsize.width());
      geom.setHeight(fsize.height());
      move(geom.topLeft());
      resize(geom.size());
    } else {
      move(geom_from_last_program_run.topLeft());
      resize(geom_from_last_program_run.size());
    }
  }
  connect(this, SIGNAL(RecMessage(const eudaq::LogMessage &)), this,
	  SLOT(AddMessage(const eudaq::LogMessage &)));
  try {
    if (filename != "")
      LoadFile(filename);
  } catch (const std::runtime_error &) {
    // probably file not found: ignore
  }
}

LogCollectorGUI::~LogCollectorGUI(){
  QSettings settings("EUDAQ collaboration", "EUDAQ");
  settings.beginGroup("MainWindowEuLog");
  settings.setValue("size", size());
  settings.setValue("pos", pos());
  settings.endGroup();
}


void LogCollectorGUI::LoadFile(const std::string &filename) {
  std::vector<std::string> sources = m_model.LoadFile(filename);
  std::cout << "File loaded, sources = " << sources.size() << std::endl;
  for (size_t i = 0; i < sources.size(); ++i) {
    size_t dot = sources[i].find('.');
    if (dot != std::string::npos) {
      AddSender(sources[i].substr(0, dot), sources[i].substr(dot + 1));
    } else {
      AddSender(sources[i]);
    }
  }
}

namespace {
  // 12-character local timestamp used for the $D field of the file pattern.
  std::string CurrentTime12(){
    std::time_t time_now = std::time(nullptr);
    char time_buff[13];
    time_buff[12] = 0;
    std::strftime(time_buff, sizeof(time_buff),
		  "%y%m%d%H%M%S", std::localtime(&time_now));
    return std::string(time_buff);
  }

  // True if the FileNamer pattern contains the given field, e.g. 'R' in "$3R".
  bool PatternHasField(const std::string &pattern, char field){
    size_t i = 0;
    while((i = pattern.find('$', i)) != std::string::npos){
      ++i;
      if(i < pattern.size() && (pattern[i] == '-' || pattern[i] == '+'))
	++i;
      while(i < pattern.size() &&
	    std::isdigit(static_cast<unsigned char>(pattern[i])))
	++i;
      if(i < pattern.size() && pattern[i] == field)
	return true;
    }
    return false;
  }
}

void LogCollectorGUI::OpenLogFile(uint32_t run_number, bool rename_current){
  std::string filename(eudaq::FileNamer(m_file_pattern)
		       .Set('D', m_start_time).Set('R', run_number));
  std::unique_lock<std::mutex> lock(m_os_file_mutex);
  if(filename == m_current_filename)
    return;
  if(m_os_file.is_open())
    m_os_file.close();
  // Carry the already-written messages (init/configure) over to the
  // run-numbered file instead of leaving them in the provisional one.
  if(rename_current && !m_current_filename.empty())
    std::rename(m_current_filename.c_str(), filename.c_str());
  m_os_file.open(filename.c_str(), std::ios_base::app);
  m_current_filename = filename;
}

void LogCollectorGUI::DoInitialise(){
  auto ini = GetInitConfiguration();
  std::string file_pattern = "euLog_$12D.log";
  if(ini){
    file_pattern = ini->Get("EULOG_GUI_LOG_FILE_PATTERN", file_pattern);
  }
  m_file_pattern = file_pattern;
  m_start_time = CurrentTime12();
  // The run number is only known at StartRun, so open a provisional file now
  // (run 0); OnStartRun renames it to the run-numbered name once it is known.
  OpenLogFile(0, false);
  m_provisional = true;
  std::unique_lock<std::mutex> lock(m_os_file_mutex);
  m_os_file << "\n*** LogCollector started at " << std::time(nullptr)
	    << " ***" << std::endl;
}

void LogCollectorGUI::OnStartRun(){
  // The run number is available now: switch the log file to the run-numbered
  // name. The first run inherits the provisional file (keeping the init and
  // configure messages); later runs in the same session get a fresh file.
  if(PatternHasField(m_file_pattern, 'R')){
    OpenLogFile(GetRunNumber(), m_provisional);
    m_provisional = false;
  }
  eudaq::LogCollector::OnStartRun();
}


void LogCollectorGUI::DoConnect(eudaq::ConnectionSPC id){
  eudaq::mSleep(100);
  CheckRegistered();
  EUDAQ_INFO("Connection from " + to_string(id));
  AddSender(id->GetType(), id->GetName());
}

void LogCollectorGUI::DoReceive(const eudaq::LogMessage &msg){

  std::cout<< msg<<std::endl;
  CheckRegistered();
  {
    std::unique_lock<std::mutex> lock(m_os_file_mutex);
    if(m_os_file.is_open())
      m_os_file << msg << std::endl;
  }
  emit RecMessage(msg);
}

void LogCollectorGUI::closeEvent(QCloseEvent *) {
  std::cout << "Closing!" << std::endl;
  QApplication::quit();
}

void LogCollectorGUI::DoTerminate() {
  std::cout << "Closing!" << std::endl;
  QApplication::quit();
}

void LogCollectorGUI::AddSender(const std::string &type,
                                const std::string &name) {
  bool foundtype = false;
  int count = cmbFrom->count();
  for (int i = 0; i <= count; ++i) {
    std::string curname,
      curtype = (i == count ? "" : cmbFrom->itemText(i).toStdString());
    size_t dot = curtype.find('.');
    if (dot != std::string::npos) {
      curname = curtype.substr(dot + 1);
      curtype = curtype.substr(0, dot);
    }
    if (curtype == type) {
      if (curname == name || (curname == "*" && name == ""))
        return;
      if (!foundtype) {
        if (curname == "" && name != "") {
          cmbFrom->setItemText(i, (curtype + ".*").c_str());
        }
        foundtype = true;
      }
    } else {
      bool insertedtype = false;
      if (i == count && !foundtype) {
        std::string text = type;
        if (name != "")
          text += ".*";
        cmbFrom->insertItem(i, text.c_str());
        insertedtype = true;
      }
      if (foundtype || (i == count && name != "")) {
        cmbFrom->insertItem(i + insertedtype, (type + "." + name).c_str());
        return;
      }
    }
  }
}

void LogCollectorGUI::on_cmbLevel_currentIndexChanged(int index) {
  m_model.SetDisplayLevel(index);
}

void LogCollectorGUI::on_cmbFrom_currentIndexChanged(const QString &text) {
  std::string type = text.toStdString(), name;
  size_t dot = type.find('.');
  if (dot != std::string::npos) {
    name = type.substr(dot + 1);
    type = type.substr(0, dot);
  }
  m_model.SetDisplayNames(type, name);
}

void LogCollectorGUI::on_txtSearch_editingFinished() {
  m_model.SetSearch(txtSearch->displayText().toStdString());
}

void LogCollectorGUI::on_viewLog_activated(const QModelIndex &i) {
  new LogDialog(m_model.GetMessage(i.row()));
}

void LogCollectorGUI::AddMessage(const eudaq::LogMessage &msg) {
  QModelIndex pos = m_model.AddMessage(msg);
  if (pos.isValid())
    viewLog->scrollTo(pos);
}

void LogCollectorGUI::CheckRegistered(){
  static bool registered = false;
  if (!registered) {
    qRegisterMetaType<QModelIndex>("QModelIndex");
    qRegisterMetaType<eudaq::LogMessage>("eudaq::LogMessage");
    registered = true;
  }
}

void LogCollectorGUI::Exec(){
  StartLogCollector(); //TODO: Start it OnServer
  Connect();
  show();
  if(QApplication::instance())
    QApplication::instance()->exec(); 
  else
    std::cerr<<"ERROR: LogCollectorGUI::EXEC\n";

  while(IsConnected() || IsActiveLogCollector()){
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}
