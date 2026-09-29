let f x = let tmp = x + 1 in tmp
module M : sig val v : int end = struct let v = 1 end
module type S = sig type u val g : u -> u end
type after = A | B
