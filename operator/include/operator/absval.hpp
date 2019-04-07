#ifndef __ABSVAL_HPP__
#define __ABSVAL_HPP__
#include "operator.hpp"
namespace TEngine {
class AbsVal: public OperatorNoParam<AbsVal> {
public:
    AbsVal() { name_="AbsVal"; }
    AbsVal(const AbsVal&)= default;
    ~AbsVal() {}
    void SetSchema(void) override;
};
} //namespace TEngine
#endif
