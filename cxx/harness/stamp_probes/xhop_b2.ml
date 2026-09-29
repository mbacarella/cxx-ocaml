module type T = sig type t end
module P (B : sig end) (O : T) = struct end
