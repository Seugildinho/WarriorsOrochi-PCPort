#pragma once
class PS2Runtime;
bool installOrochiOverlayFunctions(PS2Runtime &runtime);
bool installOrochiSupplementalFunctions(PS2Runtime &runtime);
int runOverlayRegressionTests();
