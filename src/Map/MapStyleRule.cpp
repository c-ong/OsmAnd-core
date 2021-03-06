#include "MapStyleRule.h"
#include "MapStyleRule_P.h"

#include <cassert>

#include "MapStyle.h"
#include "MapStyle_P.h"
#include "MapStyleValueDefinition.h"
#include "MapStyleValue.h"
#include "Logging.h"
#include "Utilities.h"

OsmAnd::MapStyleRule::MapStyleRule(MapStyle* owner_, const QHash< QString, QString >& attributes)
    : _d(new MapStyleRule_P(this))
    , owner(owner_)
{
    _d->_valueDefinitionsRefs.reserve(attributes.size());
    _d->_values.reserve(attributes.size());
    
    for(auto itAttribute = attributes.cbegin(); itAttribute != attributes.cend(); ++itAttribute)
    {
        const auto& key = itAttribute.key();
        const auto& value = itAttribute.value();

        std::shared_ptr<const MapStyleValueDefinition> valueDef;
        bool ok = owner->resolveValueDefinition(key, valueDef);
        assert(ok);

        _d->_valueDefinitionsRefs.push_back(valueDef);
        MapStyleValue parsedValue;
        switch (valueDef->dataType)
        {
        case MapStyleValueDataType::Boolean:
            parsedValue.asSimple.asInt = (value == QLatin1String("true")) ? 1 : 0;
            break;
        case MapStyleValueDataType::Integer:
            {
                if(valueDef->isComplex)
                {
                    parsedValue.isComplex = true;
                    if(!value.contains(':'))
                    {
                        parsedValue.asComplex.asInt.dip = Utilities::parseArbitraryInt(value, -1);
                        parsedValue.asComplex.asInt.px = 0.0;
                    }
                    else
                    {
                        // 'dip:px' format
                        const auto& complexValue = value.split(':', QString::KeepEmptyParts);

                        parsedValue.asComplex.asInt.dip = Utilities::parseArbitraryInt(complexValue[0], 0);
                        parsedValue.asComplex.asInt.px = Utilities::parseArbitraryInt(complexValue[1], 0);
                    }
                }
                else
                {
                    assert(!value.contains(':'));
                    parsedValue.asSimple.asInt = Utilities::parseArbitraryInt(value, -1);
                }
            }
            break;
        case MapStyleValueDataType::Float:
            {
                if(valueDef->isComplex)
                {
                    parsedValue.isComplex = true;
                    if(!value.contains(':'))
                    {
                        parsedValue.asComplex.asFloat.dip = Utilities::parseArbitraryFloat(value, -1.0f);
                        parsedValue.asComplex.asFloat.px = 0.0f;
                    }
                    else
                    {
                        // 'dip:px' format
                        const auto& complexValue = value.split(':', QString::KeepEmptyParts);

                        parsedValue.asComplex.asFloat.dip = Utilities::parseArbitraryFloat(complexValue[0], 0);
                        parsedValue.asComplex.asFloat.px = Utilities::parseArbitraryFloat(complexValue[1], 0);
                    }
                }
                else
                {
                    assert(!value.contains(':'));
                    parsedValue.asSimple.asFloat = Utilities::parseArbitraryFloat(value, -1.0f);
                }
            }
            break;
        case MapStyleValueDataType::String:
            parsedValue.asSimple.asUInt = owner->_d->lookupStringId(value);
            break;
        case MapStyleValueDataType::Color:
            {
                assert(value[0] == '#');
                parsedValue.asSimple.asUInt = value.mid(1).toUInt(nullptr, 16);
                if(value.size() <= 7)
                    parsedValue.asSimple.asUInt |= 0xFF000000;
            }
            break;
        }
        
        _d->_values.insert(key, parsedValue);
    }
}

OsmAnd::MapStyleRule::~MapStyleRule()
{
}

bool OsmAnd::MapStyleRule::getAttribute( const QString& key, MapStyleValue& value ) const
{
    auto itValue = _d->_values.constFind(key);
    if(itValue == _d->_values.cend())
        return false;

    value = *itValue;
    return true;
}

void OsmAnd::MapStyleRule::dump( const QString& prefix /*= QString()*/ ) const
{
    auto newPrefix = prefix + "\t";
    
    for(auto itValueDef = _d->_valueDefinitionsRefs.cbegin(); itValueDef != _d->_valueDefinitionsRefs.cend(); ++itValueDef)
    {
        auto valueDef = *itValueDef;

        MapStyleValue value;
        if(!getAttribute(valueDef->name, value))
        {
            switch (valueDef->dataType)
            {
            case MapStyleValueDataType::Boolean:
                value.asSimple.asInt = 0;
                break;
            case MapStyleValueDataType::Integer:
                value.asSimple.asInt = -1;
                break;
            case MapStyleValueDataType::Float:
                value.asSimple.asFloat = -1.0f;
                break;
            case MapStyleValueDataType::String:
                value.asSimple.asUInt = std::numeric_limits<unsigned int>::max();
                break;
            case MapStyleValueDataType::Color:
                value.asSimple.asUInt = 0;
                break;
            }
        }

        QString strValue;
        switch (valueDef->dataType)
        {
        case MapStyleValueDataType::Boolean:
            strValue = (value.asSimple.asInt == 1) ? "true" : "false";
            break;
        case MapStyleValueDataType::Integer:
            if(value.isComplex)
                strValue = QString("%1:%2").arg(value.asComplex.asInt.dip).arg(value.asComplex.asInt.px);
            else
                strValue = QString("%1").arg(value.asSimple.asInt);
            break;
        case MapStyleValueDataType::Float:
            if(value.isComplex)
                strValue = QString("%1:%2").arg(value.asComplex.asFloat.dip).arg(value.asComplex.asFloat.px);
            else
                strValue = QString("%1").arg(value.asSimple.asFloat);
            break;
        case MapStyleValueDataType::String:
            strValue = owner->_d->lookupStringValue(value.asSimple.asUInt);
            break;
        case MapStyleValueDataType::Color:
            {
                auto color = value.asSimple.asUInt;
                if((color & 0xFF000000) == 0xFF000000)
                    strValue = '#' + QString::number(color, 16).right(6);
                else
                    strValue = '#' + QString::number(color, 16).right(8);
            }
            break;
        }

        OsmAnd::LogPrintf(LogSeverityLevel::Debug, "%s%s%s = %s",
            newPrefix.toStdString().c_str(),
            (valueDef->valueClass == MapStyleValueClass::Input) ? ">" : "<",
            valueDef->name.toStdString().c_str(),
            strValue.toStdString().c_str());
    }

    if(!_d->_ifChildren.empty())
    {
        OsmAnd::LogPrintf(LogSeverityLevel::Debug, "%sIf(",
            newPrefix.toStdString().c_str());
        for(auto itChild = _d->_ifChildren.cbegin(); itChild != _d->_ifChildren.cend(); ++itChild)
        {
            auto child = *itChild;

            OsmAnd::LogPrintf(LogSeverityLevel::Debug, "%sAND",
                newPrefix.toStdString().c_str());
            child->dump(newPrefix);
        }
        OsmAnd::LogPrintf(LogSeverityLevel::Debug, "%s)",
            newPrefix.toStdString().c_str());
    }

    if(!_d->_ifElseChildren.empty())
    {
        OsmAnd::LogPrintf(LogSeverityLevel::Debug, "%sSelector: [",
            newPrefix.toStdString().c_str());
        for(auto itChild = _d->_ifElseChildren.cbegin(); itChild != _d->_ifElseChildren.cend(); ++itChild)
        {
            auto child = *itChild;

            OsmAnd::LogPrintf(LogSeverityLevel::Debug, "%sOR",
                newPrefix.toStdString().c_str());
            child->dump(newPrefix);
        }
        OsmAnd::LogPrintf(LogSeverityLevel::Debug, "%s]",
            newPrefix.toStdString().c_str());
    }
}
