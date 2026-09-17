let r = ref 0;;
let o = object val x = 33 method m = x end in
  let o' : <m:int> = Obj.magic 0 in
  assert ((o#m, o'#m) = (33, 33));;
