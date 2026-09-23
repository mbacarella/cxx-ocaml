let f x = let tmp = x + 1 in tmp
type r = R of { a : int }
module N = struct let v = 1 end
module M : sig val v : int end = N
type after = A | B
