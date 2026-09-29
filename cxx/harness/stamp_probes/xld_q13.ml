module M : sig type t = A end = struct type t = A let v = 1 end
module N = struct type s = { y : int } end
type u = { x : int }
