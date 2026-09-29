module A = struct type t = A | B end
module A2 = A
include A2
