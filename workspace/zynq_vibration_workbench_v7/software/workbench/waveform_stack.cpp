#include "waveform_stack.hpp"
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QTouchEvent>
#include <QContextMenuEvent>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>

namespace vibration::workbench {
namespace {
const char* mimeType = "application/x-vibration-wave-layout";
const std::array<QColor,3> colors{QColor("#3769c7"),QColor("#268878"),QColor("#b78032")};
class CompactMetrics final : public QLabel {
public:
    explicit CompactMetrics(QWidget* parent=nullptr):QLabel(parent){setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);setMinimumWidth(0);setAlignment(Qt::AlignRight|Qt::AlignVCenter);}
    void setText(const QString& value){full_=value;fit();}
protected:
    void resizeEvent(QResizeEvent* event)override{QLabel::resizeEvent(event);fit();}
private:
    QString full_;
    void fit(){QLabel::setText(fontMetrics().elidedText(full_,Qt::ElideRight,std::max(0,width())));}
};
class DragButton final : public QPushButton {
public:
    DragButton(QString text, QByteArray payload, QWidget* parent=nullptr)
        : QPushButton(std::move(text),parent), payload_(std::move(payload)) {
        setProperty("waveDragPayload", payload_);
        setCursor(Qt::OpenHandCursor); setMinimumHeight(44);
    }
protected:
    void mousePressEvent(QMouseEvent* e) override {
        if(e->button()==Qt::LeftButton)origin_=e->pos();
        QPushButton::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if((e->buttons()&Qt::LeftButton)&&(e->pos()-origin_).manhattanLength()>=QApplication::startDragDistance()) {
            setDown(false); auto* data=new QMimeData; data->setData(mimeType,payload_);
            // The source chip may disappear when its last channel is moved.
            // Keep the nested drag object owned by the stable window instead.
            QPointer<QDrag> drag=new QDrag(window());drag->setMimeData(data);drag->setPixmap(grab());
            drag->setHotSpot(QPoint(width()/2,height()/2));
            drag->exec(Qt::MoveAction);if(drag)drag->deleteLater();
            return;
        }
        QPushButton::mouseMoveEvent(e);
    }
private:
    QPoint origin_; QByteArray payload_;
};
QIcon gripIcon() {
    QPixmap pix(20,20);pix.fill(Qt::transparent);QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);p.setPen(Qt::NoPen);p.setBrush(QColor("#8f8f8f"));
    for(int x:{7,13})for(int y:{5,10,15})p.drawEllipse(QPointF(x,y),1.2,1.2);
    return QIcon(pix);
}
}
struct WaveformStack::Impl {
    explicit Impl(WaveformStack* owner):q(owner),token(QUuid::createUuid().toByteArray(QUuid::WithoutBraces)){}
    WaveformStack* q; QByteArray token; QWidget* content=nullptr; QVBoxLayout* rows=nullptr;
    struct Panel {QFrame* frame=nullptr; PlotCanvas* plot=nullptr; QLabel* title=nullptr; QLabel* unit=nullptr; CompactMetrics* metrics=nullptr;
        QWidget* chips=nullptr; QHBoxLayout* chipRow=nullptr; QVector<int> channels; QString chartUnit; bool initialized=false;};
    std::array<Panel,5> panels; QVector<int> order{0,1,2,3,4}; QVector<PlotSeries> sources;PlotSeries speedSource;
    QVector<QPointF> amplitudeStats;QString statsScope,speedUnit="RPM",speedSummary,speedDetail;
    QString unit="g"; std::optional<QPointF> range;
    QTimer* scrollTimer=nullptr; int scrollDirection=0;
    bool touching=false,touchScrolled=false; QPointF touchOrigin; int touchScrollStart=0;
    QTimer* touchMenuTimer=nullptr;QPointer<PlotCanvas> touchCanvas;QPointF touchLocal;
    QByteArray payload(const char* kind,int id) const {return token+":"+kind+":"+QByteArray::number(id);}
    bool decode(const QMimeData* data,QString& kind,int& id,int& channel) const {
        if(!data||!data->hasFormat(mimeType))return false;
        const auto fields=data->data(mimeType).split(':');bool ok=false;
        if(fields.size()<3||fields[0]!=token)return false;
        kind=QString::fromLatin1(fields[1]);id=fields[2].toInt(&ok);
        if(!ok||!order.contains(id))return false;
        if(kind=="panel")return fields.size()==3;
        if(kind!="channel"||fields.size()!=4)return false;
        channel=fields[3].toInt(&ok);
        return ok&&channel>=0&&channel<3&&panels[id].channels.contains(channel);
    }
    void clearMarkers() {
        for(auto& p:panels)p.frame->setStyleSheet("QFrame#viewPanel{background:white;border:0;border-top:1px solid #ededed;}");
        scrollDirection=0;scrollTimer->stop();
    }
    void render(int id) {
        auto& p=panels[id];QVector<PlotSeries> selected;
        const QString chartUnit=id==4?speedUnit:unit;
        if(id==4){if(!speedSource.points.isEmpty())selected.push_back(speedSource);}
        else for(int channel:p.channels)if(channel<sources.size())selected.push_back(sources[channel]);
        if(!p.initialized || p.chartUnit!=chartUnit) {
            p.plot->setChart({},QStringLiteral("时间 / s"),chartUnit,selected,id==4?QStringLiteral("暂无有效测速"):QStringLiteral("打开记录查看波形"));
            p.chartUnit=chartUnit;p.initialized=true;
        } else p.plot->updateSeries(std::move(selected),false);
        if(range)p.plot->setXRange(range->x(),range->y());
        updateMetrics(id);
    }
    void updateMetrics(int id) {
        auto& p=panels[id];if(!p.metrics)return;
        if(id==4){p.metrics->setText(speedSummary);p.metrics->setToolTip(speedDetail);p.unit->setText(speedUnit);return;}
        QStringList rms,peaks,detail;
        for(int channel:p.channels)if(channel<amplitudeStats.size()) {const auto value=amplitudeStats[channel];const auto format=[](double x){return std::isfinite(x)?QString::number(x,'g',3):QStringLiteral("—");};rms<<format(value.x());peaks<<format(value.y());detail<<QStringLiteral("CH%1 RMS %2 / 峰峰值 %3 %4").arg(channel+1).arg(QString::number(value.x(),'g',9),QString::number(value.y(),'g',9),unit);}
        p.metrics->setText(rms.isEmpty()?QString():QStringLiteral("RMS %1 · 峰峰 %2").arg(rms.join(" / "),peaks.join(" / ")));
        p.metrics->setToolTip(statsScope+QStringLiteral("\n")+detail.join(QStringLiteral("\n")));p.unit->setText(unit);
    }
    void rebuildChips(int id) {
        auto& p=panels[id];
        // Defer deletion: a remove button can be the sender of this rebuild.
        while(auto* item=p.chipRow->takeAt(0)){if(auto* w=item->widget()){w->hide();w->setObjectName({});for(auto* child:w->findChildren<QWidget*>())child->setObjectName({});w->deleteLater();}delete item;}
        p.title->setText(id==4?QStringLiteral("转速"):p.channels.size()==1?QString():id==3&&p.channels.size()==3?QStringLiteral("三轴叠加"):QStringLiteral("叠加"));p.title->setVisible(!p.title->text().isEmpty());
        for(int channel:p.channels) {
            auto* chip=new QWidget;chip->setObjectName(QString("waveChip%1_%2").arg(id).arg(channel));
            auto* layout=new QHBoxLayout(chip);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
            auto* drag=new DragButton(QString("CH%1").arg(channel+1),payload("channel",id)+":"+QByteArray::number(channel),chip);
            drag->setObjectName(QString("waveDragChannel%1_%2").arg(id).arg(channel));
            drag->setAccessibleName(QStringLiteral("拖动 CH%1 到其他波形栏以叠加").arg(channel+1));
            drag->setToolTip(drag->accessibleName());drag->setFixedWidth(54);
            drag->setStyleSheet(QString("QPushButton{color:%1;background:transparent;border:0;padding:0;font-size:14px;font-weight:600;}QPushButton:hover{background:#f3f6fa;}").arg(colors[channel].name()));
            layout->addWidget(drag);
            if(p.channels.size()>1) {
                auto* remove=new QPushButton(QStringLiteral("×"),chip);remove->setFixedSize(44,44);
                remove->setObjectName(QString("waveRemoveChannel%1_%2").arg(id).arg(channel));
                remove->setAccessibleName(QStringLiteral("将 CH%1 拆出为独立栏").arg(channel+1));remove->setToolTip(remove->accessibleName());remove->setEnabled(order.size()<5);
                remove->setStyleSheet("QPushButton{border:0;padding:0;background:transparent;color:#687586;font-size:13px;}QPushButton:hover{background:#e8eff8;}");
                layout->addWidget(remove);QObject::connect(remove,&QPushButton::clicked,q,[this,id,channel]{q->splitChannel(id,channel);});
            }
            p.chipRow->addWidget(chip);
        }
    }
    void changed(int id) {rebuildChips(id);render(id);if(q->layoutChanged)q->layoutChanged();}
};
WaveformStack::WaveformStack(QWidget* parent):QScrollArea(parent),impl_(std::make_unique<Impl>(this)) {
    auto& d=*impl_;setObjectName("waveformStack");setFrameShape(QFrame::NoFrame);setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setStyleSheet("QScrollArea#waveformStack{background:white;border:0;}");
    d.content=new QWidget;d.content->setObjectName("waveformStackContent");d.content->setStyleSheet("background:white;");
    d.rows=new QVBoxLayout(d.content);d.rows->setContentsMargins(0,0,8,0);d.rows->setSpacing(12);
    d.scrollTimer=new QTimer(this);d.scrollTimer->setInterval(30);
    d.touchMenuTimer=new QTimer(this);d.touchMenuTimer->setSingleShot(true);d.touchMenuTimer->setInterval(550);
    connect(d.touchMenuTimer,&QTimer::timeout,this,[this]{auto& state=*impl_;if(state.touching&&!state.touchScrolled&&state.touchCanvas){state.touching=false;QContextMenuEvent menu(QContextMenuEvent::Other,state.touchLocal.toPoint(),state.touchCanvas->mapToGlobal(state.touchLocal.toPoint()));QApplication::sendEvent(state.touchCanvas,&menu);}});
    connect(d.scrollTimer,&QTimer::timeout,this,[this]{auto* bar=verticalScrollBar();bar->setValue(bar->value()+impl_->scrollDirection*9);});
    for(int id=0;id<5;++id) {
        auto& p=d.panels[id];p.channels=id==4?QVector<int>{}:id==3?QVector<int>{0,1,2}:QVector<int>{id};
        p.frame=new QFrame; p.frame->setObjectName("viewPanel");p.frame->setProperty("stablePanelId",id);
        p.frame->setFixedHeight(208);p.frame->setAcceptDrops(true);p.frame->installEventFilter(this);
        auto* body=new QVBoxLayout(p.frame);body->setContentsMargins(0,0,0,4);body->setSpacing(0);
        auto* header=new QWidget;header->setFixedHeight(48);auto* row=new QHBoxLayout(header);
        row->setContentsMargins(0,2,8,2);row->setSpacing(7);
        auto* handle=new DragButton({},d.payload("panel",id),header);handle->setFixedSize(30,44);handle->setIcon(gripIcon());handle->setIconSize(QSize(16,16));
        handle->setObjectName(QString("waveMovePanel%1").arg(id));handle->setAccessibleName(QStringLiteral("拖动调整波形栏顺序"));handle->setToolTip(handle->accessibleName());
        handle->setStyleSheet("QPushButton{background:transparent;border:0;padding:0;color:#687586;font-size:13px;}QPushButton:hover{background:#e8eff8;}");row->addWidget(handle);
        p.title=new QLabel;p.title->setStyleSheet("font-size:14px;color:#344255;font-weight:600;");row->addWidget(p.title);
        p.chips=new QWidget;p.chipRow=new QHBoxLayout(p.chips);p.chipRow->setContentsMargins(0,0,0,0);p.chipRow->setSpacing(4);row->addWidget(p.chips);
        p.metrics=new CompactMetrics;p.metrics->setObjectName(QString("waveMetrics%1").arg(id));p.metrics->setStyleSheet("font-size:12px;color:#687586;");row->addWidget(p.metrics,1);
        p.unit=new QLabel(d.unit);p.unit->setObjectName(QString("waveUnit%1").arg(id));p.unit->setStyleSheet("font-size:12px;color:#687586;");row->addWidget(p.unit);body->addWidget(header);
        p.plot=new PlotCanvas;p.plot->setObjectName(id==4?QString("workbenchWaveSpeed"):id==3?QString("workbenchWaveOverlay"):QString("workbenchWaveCH%1").arg(id+1));
        p.plot->setProperty("stablePanelId",id);p.plot->setCompact(true);p.plot->setEmbedded(true);p.plot->setLegendVisible(false);p.plot->setXAxisVisible(true);p.plot->setZeroCenteredY(id<4);
        p.plot->setAttribute(Qt::WA_AcceptTouchEvents);p.plot->installEventFilter(this);
        p.plot->cursorSelected=[this](double x){if(cursorSelected)cursorSelected(x);};body->addWidget(p.plot,1);d.rows->addWidget(p.frame);d.rebuildChips(id);
        p.plot->navigationStarted=[this]{if(navigationStarted)navigationStarted();};
        p.plot->xRangeRequested=[this](double first,double last){if(xRangeRequested)xRangeRequested(first,last);else setXRange(first,last);};
    }
    d.rows->addStretch();setWidget(d.content);d.clearMarkers();
}
WaveformStack::~WaveformStack()=default;
PlotCanvas* WaveformStack::plot(int id) const {return id>=0&&id<5?impl_->panels[id].plot:nullptr;}
QVector<int> WaveformStack::panelOrder() const {return impl_->order;}
QVector<int> WaveformStack::panelChannels(int id) const {return id>=0&&id<5?impl_->panels[id].channels:QVector<int>{};}
void WaveformStack::setSourceSeries(const QVector<PlotSeries>& series,const QString& unit){impl_->sources=series;impl_->unit=unit;for(int id=0;id<4;++id){impl_->panels[id].unit->setText(unit);impl_->render(id);}}
void WaveformStack::setSpeedSeries(const PlotSeries& series,const QString& unit){impl_->speedSource=series;impl_->speedUnit=unit;impl_->render(4);}
void WaveformStack::setAmplitudeStats(const QVector<QPointF>& values,const QString& scope){impl_->amplitudeStats=values;impl_->statsScope=scope;for(int id=0;id<4;++id)impl_->updateMetrics(id);}
void WaveformStack::setSpeedSummary(const QString& summary,const QString& detail){impl_->speedSummary=summary;impl_->speedDetail=detail;impl_->updateMetrics(4);}
void WaveformStack::setXRange(double start,double end){if(!std::isfinite(start)||!std::isfinite(end)||end<=start)return;impl_->range=QPointF(start,end);for(auto& p:impl_->panels)p.plot->setXRange(start,end);}
void WaveformStack::setNavigationBounds(double start,double end){for(auto& p:impl_->panels)p.plot->setNavigationBounds(start,end);}
bool WaveformStack::setYRange(double minimum,double maximum){if(!std::isfinite(minimum)||!std::isfinite(maximum)||maximum<=minimum||!std::isfinite(maximum-minimum))return false;for(int id=0;id<4;++id)impl_->panels[id].plot->setYRange(minimum,maximum);return true;}
void WaveformStack::clearYRange(){for(int id=0;id<4;++id)impl_->panels[id].plot->clearYRange();}
void WaveformStack::setNavigationMode(PlotCanvas::NavigationMode mode){for(auto& p:impl_->panels)p.plot->setNavigationMode(mode);}
void WaveformStack::clearData(){impl_->sources.clear();impl_->speedSource={};impl_->amplitudeStats.clear();impl_->speedSummary.clear();impl_->statsScope.clear();impl_->range.reset();for(int id=0;id<5;++id){impl_->render(id);impl_->panels[id].plot->clearXRange();impl_->panels[id].plot->clearYRange();impl_->panels[id].plot->clearNavigationBounds();impl_->panels[id].plot->setCursor(std::nullopt);}}
bool WaveformStack::movePanel(int id,int destination){
    auto& d=*impl_;if(!d.order.contains(id)||destination<0||destination>=d.order.size())return false;
    const int from=d.order.indexOf(id);if(from==destination)return false;
    d.order.removeAt(from);d.order.insert(destination,id);d.rows->removeWidget(d.panels[id].frame);d.rows->insertWidget(destination,d.panels[id].frame);
    if(layoutChanged)layoutChanged();return true;
}
bool WaveformStack::addChannel(int id,int channel){if(id==4||!impl_->order.contains(id)||channel<0||channel>2)return false;auto& p=impl_->panels[id];if(p.channels.contains(channel))return false;p.channels.push_back(channel);std::sort(p.channels.begin(),p.channels.end());impl_->changed(id);return true;}
bool WaveformStack::removeChannel(int id,int channel){if(!impl_->order.contains(id))return false;auto& p=impl_->panels[id];if(p.channels.size()<=1||!p.channels.contains(channel))return false;p.channels.removeOne(channel);impl_->changed(id);return true;}
bool WaveformStack::moveChannel(int source,int target,int channel){
    auto& d=*impl_;if(source==4||target==4||source==target||!d.order.contains(source)||!d.order.contains(target)||!d.panels[source].channels.contains(channel))return false;
    auto& destination=d.panels[target].channels;if(!destination.contains(channel)){destination.push_back(channel);std::sort(destination.begin(),destination.end());}
    d.panels[source].channels.removeOne(channel);
    if(d.panels[source].channels.isEmpty()){d.order.removeOne(source);d.rows->removeWidget(d.panels[source].frame);d.panels[source].frame->hide();}
    for(int id=0;id<5;++id){d.rebuildChips(id);d.render(id);}if(layoutChanged)layoutChanged();return true;
}
bool WaveformStack::splitChannel(int source,int channel){
    auto& d=*impl_;if(source==4||!d.order.contains(source)||d.panels[source].channels.size()<2||!d.panels[source].channels.contains(channel)||d.order.size()>=5)return false;
    int free=0;while(d.order.contains(free))++free;
    d.panels[source].channels.removeOne(channel);d.panels[free].channels={channel};
    const int index=d.order.indexOf(source)+1;d.order.insert(index,free);d.rows->insertWidget(index,d.panels[free].frame);d.panels[free].frame->show();
    for(int id=0;id<5;++id){d.rebuildChips(id);d.render(id);}if(layoutChanged)layoutChanged();return true;
}
void WaveformStack::resetLayout(){auto& d=*impl_;d.order={0,1,2,3,4};for(int id=0;id<5;++id){d.rows->removeWidget(d.panels[id].frame);d.rows->insertWidget(id,d.panels[id].frame);d.panels[id].frame->show();d.panels[id].channels=id==4?QVector<int>{}:id==3?QVector<int>{0,1,2}:QVector<int>{id};d.rebuildChips(id);d.render(id);}verticalScrollBar()->setValue(0);if(layoutChanged)layoutChanged();}
bool WaveformStack::eventFilter(QObject* object,QEvent* event){
    auto& d=*impl_;auto* widget=qobject_cast<QWidget*>(object);if(!widget)return QScrollArea::eventFilter(object,event);
    const int target=widget->property("stablePanelId").toInt();
    if(auto* canvas=dynamic_cast<PlotCanvas*>(widget)) {
        if(event->type()==QEvent::TouchBegin||event->type()==QEvent::TouchUpdate||event->type()==QEvent::TouchEnd||event->type()==QEvent::TouchCancel) {
            auto* touch=static_cast<QTouchEvent*>(event);event->accept();
            if(event->type()==QEvent::TouchCancel){d.touching=false;d.touchMenuTimer->stop();return true;}
            if(touch->touchPoints().isEmpty())return true;
            const auto pos=touch->touchPoints().first().screenPos();
            if(event->type()==QEvent::TouchBegin){d.touching=true;d.touchScrolled=false;d.touchOrigin=pos;d.touchScrollStart=verticalScrollBar()->value();d.touchCanvas=canvas;d.touchLocal=touch->touchPoints().first().pos();d.touchMenuTimer->start();}
            else if(d.touching){const auto delta=pos-d.touchOrigin;if(std::abs(delta.y())>12)d.touchScrolled=true;
                if(delta.manhattanLength()>12||event->type()==QEvent::TouchEnd)d.touchMenuTimer->stop();
                if(d.touchScrolled)verticalScrollBar()->setValue(d.touchScrollStart-int(delta.y()));
                if(event->type()==QEvent::TouchEnd){if(!d.touchScrolled&&canvas->explicitXRange()){auto bounds=canvas->plotArea();const double x=std::clamp((touch->touchPoints().first().pos().x()-bounds.left())/std::max(1.,bounds.width()),0.,1.);auto r=*canvas->explicitXRange();canvas->selectAtX(r.x()+x*(r.y()-r.x()));}d.touching=false;}
            }return true;
        }
        return QScrollArea::eventFilter(object,event);
    }
    if(event->type()==QEvent::DragLeave){d.clearMarkers();event->accept();return true;}
    if(event->type()==QEvent::DragEnter||event->type()==QEvent::DragMove||event->type()==QEvent::Drop) {
        auto* drop=static_cast<QDropEvent*>(event);QString kind;int source=-1,channel=-1;
        if(!d.order.contains(target)||!d.decode(drop->mimeData(),kind,source,channel)||source==target||(kind=="channel"&&target==4)){d.clearMarkers();drop->ignore();return true;}
        drop->setDropAction(Qt::MoveAction);drop->accept();
        const bool after=drop->pos().y()>widget->height()/2;
        if(event->type()==QEvent::Drop){d.clearMarkers();if(kind=="channel")moveChannel(source,target,channel);else if(source!=target){int destination=d.order.indexOf(target)+(after?1:0);if(d.order.indexOf(source)<destination)--destination;movePanel(source,destination);}return true;}
        d.clearMarkers();
        widget->setStyleSheet(kind=="channel"?"QFrame#viewPanel{background:#fafafa;border:1px dashed #999999;}":QString("QFrame#viewPanel{background:white;border:0;border-%1:2px solid #343638;}").arg(after?"bottom":"top"));
        const int y=widget->mapTo(viewport(),drop->pos()).y();d.scrollDirection=y<42?-1:y>viewport()->height()-42?1:0;if(d.scrollDirection)d.scrollTimer->start();return true;
    }
    return QScrollArea::eventFilter(object,event);
}
}
