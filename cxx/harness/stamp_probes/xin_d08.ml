module A = struct module Make (M : sig end) = struct let f s = s let g s = s
end include Make (struct end) end
let x = A.f 1
let y = A.g 2
