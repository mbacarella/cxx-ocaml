module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  let f (x : S.t) = 1
end
