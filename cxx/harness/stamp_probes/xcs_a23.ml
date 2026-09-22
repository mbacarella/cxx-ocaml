module type S = sig type t = A | B end
module M : S = struct type t = A | B end
module N = M
include N
