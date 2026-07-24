#include "dsidle/epoch.h"
#include <cassert>
int main() { dsidle::EpochTable table(2,2); assert(table.MinimumActive()==dsidle::kEpochInactive); table.Enter(0,0,8); table.Enter(1,1,3); assert(table.MinimumActive()==3); table.Leave(1,1); assert(table.MinimumActive()==8); table.Leave(0,0); assert(table.MinimumActive()==dsidle::kEpochInactive); }
