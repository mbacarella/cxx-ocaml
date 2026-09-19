type z = int
module rec Typ : sig
  module type P = sig type t end  module type Q = sig type u type v end
end = struct
  module type P = sig type t end  module type Q = sig type u type v end
end
