module type S = sig type t = X | Y val f : t -> int end
module rec A : S = struct type t = X | Y let f _ = 0 end
and B : sig type t = P | Q of A.t end = struct type t = P | Q of A.t end
