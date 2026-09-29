module type S = sig
  module rec A : sig type t = X | Y of B.t end
  and B : sig type t = P | Q of A.t end
end
module M : S = struct
  module rec A : sig type t = X | Y of B.t end
    = struct type t = X | Y of B.t end
  and B : sig type t = P | Q of A.t end
    = struct type t = P | Q of A.t end
end
