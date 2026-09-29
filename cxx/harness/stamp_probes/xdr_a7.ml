module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  let g () = (S.empty : S.t)
end
