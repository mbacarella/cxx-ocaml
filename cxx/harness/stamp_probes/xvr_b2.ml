module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  exception E of S.t
end
