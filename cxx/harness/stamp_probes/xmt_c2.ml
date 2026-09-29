module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  let y = if true then S.empty else S.empty
end
