type z = int
module rec Typ : sig
  module M : sig module type P = sig type t type t1 end end
end = struct
  module M = struct module type P = sig type t type t1 end end
end
