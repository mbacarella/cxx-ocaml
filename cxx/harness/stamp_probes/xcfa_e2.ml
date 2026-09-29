type r = R of { a : int }
module N = struct let v = 1 end
module M : sig val v : int end = N
module type S = sig type u val g : u -> u val h : u -> int end
module type T = sig val k : int end
type after = A | B
