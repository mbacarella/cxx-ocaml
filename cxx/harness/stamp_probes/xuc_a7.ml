module type S = sig type t type s val v : t end
module type T1 = S with type t = int and type s = char
module type T2 = S with type t := int
module type U = sig type z end
