module M : sig type u end = struct
  module S = Set.Make (String)
  let y : S.t = S.empty
  type u = int
end
