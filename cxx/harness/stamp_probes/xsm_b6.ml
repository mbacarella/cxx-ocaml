type z = int
module rec Typ : sig
  module type P = sig module M : sig type t end end
end = struct
  module type P = sig module M : sig type t end end
end
