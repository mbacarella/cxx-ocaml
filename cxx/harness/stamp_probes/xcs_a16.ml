module A = struct type t = A | B end
module B = struct type t = A | B end
include A
include B
