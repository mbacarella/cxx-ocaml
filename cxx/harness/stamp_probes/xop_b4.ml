module M : sig type u end = struct
  module S = Map.Make (String)
  type u = int
  let y = if true then S.(empty) else S.(empty)
end
