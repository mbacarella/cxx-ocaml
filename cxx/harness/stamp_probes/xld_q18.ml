module type T = sig type t = A module type S = sig type q = C end end
module N = struct type s = { y : int } end
