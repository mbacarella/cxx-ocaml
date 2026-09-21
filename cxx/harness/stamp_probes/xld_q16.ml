module type T = sig type t = A end
module N = struct type s = { y : int } end
module M : sig type t = A end = struct type t = A let v = 1 end
type w = { z : int }
