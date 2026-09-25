module M = struct type e = A and f = F of e * g and g = G of f | H end
let k (M.F (_, g)) = g
