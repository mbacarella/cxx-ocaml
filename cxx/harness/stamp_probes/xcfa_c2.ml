let f x = let tmp = x + 1 in tmp
module N = struct let v = 1 end
module M : sig val v : int end = struct include N end
type after = { fst : int; snd : int }
