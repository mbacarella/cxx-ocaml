let f x = let tmp = x + 1 in tmp
module N = struct type t = int let v = 1 end
module M : sig val v : int end = N
type after = A | B
