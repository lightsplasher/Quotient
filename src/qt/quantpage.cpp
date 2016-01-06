#include "quantpage.h"
#include "ui_quantpage.h"

#include "guiutil.h"
#include "guiconstants.h"

#include <algorithm>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/foreach.hpp>
#include <map>

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "net.h"
#include "db.h"

#include <QAbstractItemDelegate>
#include <QPainter>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QtScript/QScriptEngine>
#include <QtScript/QScriptValue>
#include <QtScript/QScriptValueIterator>

using namespace std;

bool isClosing = false;
qsreal trexLastPrice = 0;

CCriticalSection cs_markets;
map<qsreal, qsreal> mapBuys;
map<qsreal, qsreal> mapSells;
QVector<double> timeData(0), priceData(0), volumeData(0);
QVector<double> depthBuyPriceData(0), depthBuySumData(0);
QVector<double> depthSellPriceData(0), depthSellSumData(0);

QuantPage::QuantPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::QuantPage)
{
    ui->setupUi(this);
    networkManager = new QNetworkAccessManager(this);
    updateMarketData();
    connect(&updateTimer, SIGNAL(timeout()), this, SLOT(updateTimer_timeout()));
    updateTimer.setInterval(300000); // every 5 minutes
    updateTimer.start();
}

void QuantPage::updateTimer_timeout()
{
    if(fShutdown)
	return;

    if(ui->checkBox->isChecked())
        updateMarketData();
}

void QuantPage::updateChart()
{
    if(fShutdown)
        return;

    ui->customPlot->clearPlottables();
    ui->customPlot->clearGraphs();
    ui->customPlot->clearItems();

    ui->volumePlot->clearPlottables();
    ui->volumePlot->clearGraphs();
    ui->volumePlot->clearItems();

    double binSize = 3600; // bin data in 300 second (5 minute)// 900 second (15 minute) intervals // 3600 second (1 hour)
    // create candlestick chart:
    double startTime = timeData[0];
    QCPFinancial *candlesticks = new QCPFinancial(ui->customPlot->xAxis, ui->customPlot->yAxis);
    ui->customPlot->addPlottable(candlesticks);
    QCPFinancialDataMap data1 = QCPFinancial::timeSeriesToOhlc(timeData, priceData, binSize, startTime);
    candlesticks->setName("Price");
    candlesticks->setChartStyle(QCPFinancial::csCandlestick);
    candlesticks->setData(&data1, true);
    candlesticks->setWidth(binSize*0.9);
    candlesticks->setTwoColored(true);
    candlesticks->setBrushPositive(QColor(72, 191, 66));
    candlesticks->setBrushNegative(QColor(203, 19, 19));
    candlesticks->setPenPositive(QPen(QColor(72, 191, 66)));
    candlesticks->setPenNegative(QPen(QColor(203, 19, 19)));

    
    // create two bar plottables, for positive (green) and negative (red) volume bars:
    QCPBars *volumePos = new QCPBars(ui->volumePlot->xAxis, ui->volumePlot->yAxis);
    QCPBars *volumeNeg = new QCPBars(ui->volumePlot->xAxis, ui->volumePlot->yAxis);

    for (int i=0; i < timeData.count(); ++i)
    {
      double v = volumeData[i];
      (v < 0 ? volumeNeg : volumePos)->addData(timeData[i], qAbs(v)); // add data to either volumeNeg or volumePos, depending on sign of v
    }

    ui->customPlot->setAutoAddPlottableToLegend(false);
    ui->volumePlot->addPlottable(volumePos);
    ui->volumePlot->addPlottable(volumeNeg);
    volumePos->setWidth(binSize * 0.8);
    QPen pen;
    pen.setWidthF(1.2);
    pen.setColor(QColor(72, 191, 66));
    volumePos->setPen(pen);
    volumePos->setBrush(QColor(72, 191, 66, 20));
    volumeNeg->setWidth(binSize * 0.8);
    pen.setColor(QColor(201, 19, 19));
    volumeNeg->setPen(pen);
    volumeNeg->setBrush(QColor(203, 19, 19, 20));
  
    // configure axes of both main and bottom axis rect:
    ui->volumePlot->xAxis->setAutoTickStep(false);
    ui->volumePlot->xAxis->setTickStep(3600 * 24); // 24 hr tickstep
    ui->volumePlot->xAxis->setTickLabelType(QCPAxis::ltDateTime);
    ui->volumePlot->xAxis->setDateTimeSpec(Qt::UTC);
    ui->volumePlot->xAxis->setDateTimeFormat("dd. MMM hh:mm");
    ui->volumePlot->xAxis->setTickLabelRotation(15);
    ui->volumePlot->xAxis->setTickLabelColor(QColor(137, 140, 146));
    ui->volumePlot->yAxis->setTickLabelColor(QColor(137, 140, 146));
    ui->volumePlot->yAxis->setAutoTickStep(false);
    ui->volumePlot->yAxis->setTickStep(3000);
    ui->volumePlot->rescaleAxes();
    ui->volumePlot->yAxis->grid()->setSubGridVisible(false);

    ui->customPlot->xAxis->setBasePen(Qt::NoPen);
    ui->customPlot->xAxis->setTickLabels(false);
    ui->customPlot->xAxis->setTicks(false); // only want vertical grid in main axis rect, so hide xAxis backbone, ticks, and labels
    ui->customPlot->xAxis->setAutoTickStep(false);
    ui->customPlot->xAxis->setTickStep(3600 * 24); // 6 hr tickstep
    ui->customPlot->rescaleAxes();
    //  ui->customPlot->xAxis->scaleRange(1.025, ui->customPlot->xAxis->range().center());
    ui->customPlot->yAxis->scaleRange(1.1, ui->customPlot->yAxis->range().center());
    ui->customPlot->xAxis->setTickLabelColor(QColor(137, 140, 146));
    ui->customPlot->yAxis->setTickLabelColor(QColor(137, 140, 146));
  
    // make axis rects' left side line up:
    QCPMarginGroup *group = new QCPMarginGroup(ui->customPlot);
    ui->customPlot->axisRect()->setMarginGroup(QCP::msLeft|QCP::msRight, group);

    QLinearGradient plotGradient;
    plotGradient.setStart(0, 0);
    plotGradient.setFinalStop(0, 350);
    plotGradient.setColorAt(0, QColor(10, 10, 10));
    plotGradient.setColorAt(1, QColor(0, 0, 0));
    ui->customPlot->setBackground(plotGradient);

    QLinearGradient volumePlotGradient;
    volumePlotGradient.setStart(0, 0);
    volumePlotGradient.setFinalStop(0, 150);
    volumePlotGradient.setColorAt(0, QColor(1, 1, 1));
    volumePlotGradient.setColorAt(1, QColor(0, 0, 0));
    ui->volumePlot->setBackground(volumePlotGradient);

    ui->customPlot->xAxis->grid()->setVisible(false);
    ui->customPlot->yAxis->grid()->setVisible(false);
    ui->customPlot->xAxis->grid()->setSubGridVisible(false);
    ui->customPlot->yAxis->grid()->setSubGridVisible(false);

    ui->customPlot->replot();
    ui->volumePlot->replot();
}

void QuantPage::updateDepthChart()
{
    if(fShutdown)
	return;

    ui->depthPlot->clearPlottables();
    ui->depthPlot->clearGraphs();
    ui->depthPlot->clearItems();
    ui->depthPlot->addGraph();
    ui->depthPlot->graph(0)->setPen(QPen(QColor(72, 191, 66))); // line color green for first graph
    ui->depthPlot->graph(0)->setBrush(QBrush(QColor(72, 191, 66, 20))); // first graph will be filled with translucent green
    ui->depthPlot->addGraph();
    ui->depthPlot->graph(1)->setPen(QPen(QColor(203, 19, 19))); // line color red for second graph
    ui->depthPlot->graph(1)->setBrush(QBrush(QColor(203, 19, 19, 20)));

    ui->depthPlot->graph(0)->setData(depthBuyPriceData, depthBuySumData);
    ui->depthPlot->graph(1)->setData(depthSellPriceData, depthSellSumData);
    ui->depthPlot->xAxis->setRangeLower(0);
    ui->depthPlot->xAxis->setRangeUpper(2 * depthBuyPriceData.last());
    double yr = depthSellSumData.last();
    if(depthBuySumData.last() > yr)
        yr = depthBuySumData.last();

    ui->depthPlot->yAxis->setRangeLower(0);
    ui->depthPlot->yAxis->setRangeUpper(yr);

    QLinearGradient plotGradient;
    plotGradient.setStart(0, 0);
    plotGradient.setFinalStop(0, 350);
    plotGradient.setColorAt(0, QColor(10, 10, 10));
    plotGradient.setColorAt(1, QColor(0, 0, 0));
    ui->depthPlot->setBackground(plotGradient);

    ui->depthPlot->xAxis->grid()->setVisible(false);
    ui->depthPlot->yAxis->grid()->setVisible(false);
    ui->depthPlot->xAxis->grid()->setSubGridVisible(false);
    ui->depthPlot->yAxis->grid()->setSubGridVisible(false);

    ui->depthPlot->xAxis->setTickLabelColor(QColor(137, 140, 146));
    ui->depthPlot->yAxis->setTickLabelColor(QColor(137, 140, 146));

    ui->depthPlot->replot();
}

void QuantPage::updateOrderBook()
{
    if(fShutdown)
        return;

    LOCK(cs_markets);
    depthBuyPriceData.clear();
    depthBuySumData.clear();
    depthSellPriceData.clear();
    depthSellSumData.clear();

    ui->askTableWidget->clearContents();
    ui->askTableWidget->setRowCount(0);
    int arow = 0;
    ui->bidTableWidget->clearContents();
    ui->bidTableWidget->setRowCount(0);
    int brow = 0;

    ui->askTableWidget->horizontalHeader()->hide();
    ui->bidTableWidget->horizontalHeader()->hide();
    ui->askTableWidget->verticalHeader()->hide();
    ui->bidTableWidget->verticalHeader()->hide();

    QFont fnt;
    fnt.setPointSize(8);
    fnt.setFamily("Monospace");

    map<double, double> sellDepthMap;

    BOOST_FOREACH(const PAIRTYPE(qsreal, qsreal)& sell, mapSells)
    {
	QString amount = QString::number(sell.second, 'f', 8);
	QString price = QString::number(sell.first, 'f', 8);
	if(sell.first > 0)
        {
	    // Add to depth map
	    if(sellDepthMap.size() == 0)
		sellDepthMap.insert(make_pair(COIN * sell.first, sell.second));
	    else
	    {
		// take the last amount and add to it, to produce running sum
		double prevSum = sellDepthMap.rbegin()->second;
		double newSum = prevSum + sell.second;
		sellDepthMap.insert(make_pair(COIN * sell.first, newSum));
	    }

	    // sell
            ui->askTableWidget->insertRow(0);
            QTableWidgetItem *newItemAM = new QTableWidgetItem(amount);
            QTableWidgetItem *newItemPR = new QTableWidgetItem(price);
            newItemAM->setFont(fnt);
    	    newItemPR->setFont(fnt);        
            ui->askTableWidget->setItem(0, 0, newItemAM);
            ui->askTableWidget->setItem(0, 1, newItemPR);
            arow++;	    
        }
    }

    map<double, double> buyDepthMap;
    // iterate buys in reverse
    BOOST_REVERSE_FOREACH(const PAIRTYPE(qsreal, qsreal)& buy, mapBuys)
    {
        if(buy.first > 0)
        {
            // Add to depth map
	    if(buyDepthMap.size() == 0)
		buyDepthMap.insert(make_pair(COIN * buy.first, buy.second));
	    else
	    {
		// take the first amount and add to it, to produce running sum
		double prevSum = buyDepthMap.begin()->second;
		double newSum = prevSum + buy.second;
		buyDepthMap.insert(make_pair(COIN * buy.first, newSum));
	    }
        }
    }

    BOOST_FOREACH(const PAIRTYPE(qsreal, qsreal)& buy, mapBuys)
    {
	QString amount = QString::number(buy.second, 'f', 8);
	QString price = QString::number(buy.first, 'f', 8);
	if(buy.first > 0)
        {
	    
            ui->bidTableWidget->insertRow(0);
            QTableWidgetItem *newItemAM = new QTableWidgetItem(amount);
            QTableWidgetItem *newItemPR = new QTableWidgetItem(price);
            newItemAM->setFont(fnt);
	    newItemPR->setFont(fnt);
            ui->bidTableWidget->setItem(0, 0, newItemAM);
            ui->bidTableWidget->setItem(0, 1, newItemPR);
            brow++;
        }
    }

    // add the sell and buy depth
    BOOST_FOREACH(const PAIRTYPE(double, double)& buy, buyDepthMap)
    {
	depthBuyPriceData.append(buy.first);
	depthBuySumData.append(buy.second);
    }

    BOOST_FOREACH(const PAIRTYPE(double, double)& sell, sellDepthMap)
    {
        depthSellPriceData.append(sell.first);
        depthSellSumData.append(sell.second);
    }

    int rowHeight = 12;
    ui->askTableWidget->verticalHeader()->setUpdatesEnabled(false); 
    for (unsigned int i = 0; i < ui->askTableWidget->rowCount(); i++)
        ui->askTableWidget->verticalHeader()->resizeSection(i, rowHeight);
    ui->askTableWidget->verticalHeader()->setUpdatesEnabled(true);

    ui->bidTableWidget->verticalHeader()->setUpdatesEnabled(false); 
    for (unsigned int i = 0; i < ui->bidTableWidget->rowCount(); i++)
        ui->bidTableWidget->verticalHeader()->resizeSection(i, rowHeight);
    ui->bidTableWidget->verticalHeader()->setUpdatesEnabled(true);

    ui->askTableWidget->scrollToBottom();
    ui->bidTableWidget->scrollToTop();

    updateDepthChart();
}

void QuantPage::on_refreshNowButton_clicked()
{
    updateMarketData();
}

void QuantPage::updateMarketData()
{
    if(fShutdown)
	return;

    ui->errorLabel->setText("");

    LOCK(cs_markets);
    mapSells.clear();
    mapBuys.clear();
    timeData.clear();
    priceData.clear();
    volumeData.clear();


    //https://bittrex.com/api/v1.1/public/getmarketsummary?market=btc-ltc
    QUrl urlBD("https://bittrex.com/api/v1.1/public/getmarketsummary?market=btc-xqn");
    QNetworkRequest requestBD;
    requestBD.setUrl(urlBD);
    QNetworkReply* currentReplyBD = networkManager->get(requestBD);
    connect(currentReplyBD, SIGNAL(finished()), this, SLOT(trexMktDataReplyFinished()));

    //https://bittrex.com/api/v1.1/public/getorderbook?market=BTC-LTC&type=both&depth=50
    QUrl urlBO("https://bittrex.com/api/v1.1/public/getorderbook?market=BTC-XQN&type=both&depth=50");
    QNetworkRequest requestBO;
    requestBO.setUrl(urlBO);
    QNetworkReply* currentReplyBO = networkManager->get(requestBO);
    connect(currentReplyBO, SIGNAL(finished()), this, SLOT(trexHistoryReplyFinished()));

    // get market history for chart
    //https://bittrex.com/api/v1.1/public/getmarkethistory?market=BTC-XQN&count=50
    QUrl urlBH("https://bittrex.com/api/v1.1/public/getmarkethistory?market=BTC-XQN&count=50");
    QNetworkRequest requestBH;
    requestBH.setUrl(urlBH);
    QNetworkReply* currentReplyBH = networkManager->get(requestBH);
    connect(currentReplyBH, SIGNAL(finished()), this, SLOT(trexMktHistoryReplyFinished()));

}

void QuantPage::trexMktDataReplyFinished()
{ 
    if(fShutdown)
	return;

    //qDebug() << "trexMktDataReplyFinished()";
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply->error() !=0)
    {
        if (reply->error() == 1 )
        {
            //QMessageBox::critical(this, "Quant Connection Problem", "Bittrex Connection Refused");
	    ui->errorLabel->setText("Bittrex Connection Refused");
            return;
        }
    else
    {
        ui->errorLabel->setText("Bittrex API: " + reply->errorString());
        //QMessageBox::critical(this, "Quant Connection Problem (Bittrex API Market Data)", reply->errorString());
        return;
    }
    }

    QString data = (QString) reply->readAll();
    QScriptEngine engine;
    QScriptValue result = engine.evaluate("value = " + data);
    
    // Now parse this JSON according to your needs !
    QScriptValue resultEntry = result.property("result");
    QScriptValueIterator it(resultEntry);

    LOCK(cs_markets);
    while(it.hasNext())
    {
        it.next();
        QScriptValue entry = it.value();

        qsreal last = entry.property("Last").toNumber(); 
        if(last > 0)
        {
            qsreal prevday = entry.property("PrevDay").toNumber();
            if(last < trexLastPrice)
            {
	        // red
                ui->trexLastPriceLabel->setObjectName("trexLastPriceLabel");
	        ui->trexLastPriceLabel->setStyleSheet("#trexLastPriceLabel { color: #FF0000; background-color:#000000; }");
   	        ui->trexLastPriceLabel->setText("B: " + QString::number(last, 'f', 8));
            }
            else if(last == trexLastPrice)
            {
	        // no change
            }
            else
            {
                ui->trexLastPriceLabel->setObjectName("trexLastPriceLabel");
   	        ui->trexLastPriceLabel->setStyleSheet("#trexLastPriceLabel { color: #00FF00; background-color:#000000; }");
	        ui->trexLastPriceLabel->setText("B: " + QString::number(last, 'f', 8));
            }

            trexLastPrice = last;
        }
    }
}

void QuantPage::trexMktHistoryReplyFinished()
{
    if(fShutdown)
	return;

    QString trexFormat = "yyyy-MM-ddThh:mm:ss.z";
    //qDebug() << "trexMktHistoryReplyFinished()";
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply->error() !=0)
    {
        if (reply->error() == 1 )
        {
            //QMessageBox::critical(this, "Quant Connection Problem", "Bittrex Connection Refused");
	    ui->errorLabel->setText("Bittrex Connection Refused");
            return;
        }
    else
    {
        //QMessageBox::critical(this, "Quant Connection Problem (Bittrex API Market History)", reply->errorString());
	qDebug() << reply->errorString();
        ui->errorLabel->setText("Bittrex: " + reply->errorString());
        return;
    }
    }

    QString data = (QString) reply->readAll();
    QScriptEngine engine;
    QScriptValue result = engine.evaluate("value = " + data);
    QScriptValueIterator it(result.property("result"));
    LOCK(cs_markets);
    while(it.hasNext())
    {
        it.next();
        QScriptValue entry = it.value();
//[{"Id":664,"TimeStamp":"2014-11-14T02:38:08.307","Quantity":171.48666612,"Price":0.00002269,"Total":0.00389103,"FillType":"PARTIAL_FILL","OrderType":"SELL"},

        QString id = entry.property("Id").toString();
	if(id != "")
	{
  	    QString rawDate = entry.property("TimeStamp").toString();
	    if(!rawDate.contains("."))
	        rawDate = rawDate + ".0";

            QDateTime dt = QDateTime::fromString(rawDate, trexFormat);
	    dt.setTimeZone(QTimeZone(0));

	    qsreal price = COIN * entry.property("Price").toNumber();
	    qsreal vol = entry.property("Quantity").toNumber();

	    QString orderType = entry.property("OrderType").toString();
	    if(orderType == "SELL")
		vol = vol * -1;

	    QDateTime dtNow = QDateTime::currentDateTimeUtc();
	    if(dt >= dtNow.addDays(-7))
	    {
    	        timeData.append(dt.toTime_t());
	        priceData.append(price);
	        volumeData.append(vol);
	    }
        }
    }

    updateChart();
}

void QuantPage::trexHistoryReplyFinished()
{
    if(fShutdown)
	return;

    //qDebug() << "trexHistoryReplyFinished()";
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply->error() !=0)
    {
        if (reply->error() == 1 )
        {
            //QMessageBox::critical(this, "Quant Connection Problem", "Bittrex Connection Refused");
	    ui->errorLabel->setText("Bittrex Connection Refused");
            return;
        }
    else
    {
        //QMessageBox::critical(this, "Quant Connection Problem (Bittrex API Order Book)", reply->errorString());
        ui->errorLabel->setText("Bittrex: " + reply->errorString());
        return;
    }
    }

    QString data = (QString) reply->readAll();
    QScriptEngine engine;
    QScriptValue result = engine.evaluate("value = " + data);
    
    QScriptValue resultEntry = result.property("result");
    QScriptValueIterator itBuy(resultEntry.property("buy"));
    while(itBuy.hasNext())
    {
        itBuy.next();
        QScriptValue entry = itBuy.value();

        qsreal qty = entry.property("Quantity").toNumber();
        qsreal rate = entry.property("Rate").toNumber();

        if(mapBuys.find(rate) != mapBuys.end())
        {
	    mapBuys[rate] += qty;
        }
	else
	{
	    mapBuys.insert(make_pair(rate, qty));
	}

    }

    QScriptValueIterator itSell(resultEntry.property("sell"));
    while(itSell.hasNext())
    {
        itSell.next();
        QScriptValue entry = itSell.value();

        qsreal qty = entry.property("Quantity").toNumber();
        qsreal rate = entry.property("Rate").toNumber();

        if(mapSells.find(rate) != mapSells.end())
        {
	    mapSells[rate] += qty;
        }
	else
	{
	    mapSells.insert(make_pair(rate, qty));
	}
    }

    updateOrderBook();
}

QuantPage::~QuantPage()
{
    isClosing = true;
    try
    {
        updateTimer.stop();
        //updateTimer = NULL;
        //delete networkManager;
        updateTimer.deleteLater();
        networkManager->deleteLater();
        delete ui;
    }
    catch(std::exception& e)
    {
	// sometimes the QCustomPlot destructor throws
        qDebug() << e.what();
    }
}
