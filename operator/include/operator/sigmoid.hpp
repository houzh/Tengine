#ifndef __SIGMOID_HPP__
#define __SIGMOID_HPP__
#include "operator.hpp"
namespace TEngine {
class Sigmoid: public OperatorNoParam<Sigmoid> {
public:
    Sigmoid() { name_="Sigmoid"; }
    Sigmoid(const Sigmoid&)= default;
    ~Sigmoid() {}
    void SetSchema(void) override;
};
} //namespace TEngine
#endif
