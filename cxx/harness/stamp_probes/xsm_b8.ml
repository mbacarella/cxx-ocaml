type z = int
module rec Typ : sig
  module M : sig type t type t1 end
end = struct
  module M = struct type t = int type t1 = int end
end
