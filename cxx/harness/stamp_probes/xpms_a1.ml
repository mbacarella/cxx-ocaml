module F (M : sig type t val f : unit -> t end) = struct
  let t = M.f ()
end
