module type MyT = sig type t module M : sig type u val v : u end end
module MyMap(X : MyT) = struct include X end
