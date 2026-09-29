module A = struct module Make (M : sig val x : int end) = struct let f s = s
end include Make (struct let x = 1 end) end
let x = A.f 1
