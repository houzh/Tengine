#ifndef __TANH_HPP__
#define __TANH_HPP__
#include "operator.hpp"
namespace TEngine {
class TanH: public OperatorNoParam<TanH> {
public:
    TanH() { name_="TanH"; }
    TanH(const TanH&)= default;
    ~TanH() {}
    void SetSchema(void) override;
};
} //namespace TEngine
#endif
