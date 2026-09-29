module M : sig type u end = struct
  module S = Set.Make (Stdlib__String)
  type u = int
end
