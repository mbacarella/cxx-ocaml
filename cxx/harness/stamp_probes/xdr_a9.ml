module M : sig type u end = struct
  module S = Set.Make (String)
  module R = Set.Make (String)
  type v = R.t
  type u = int
end
