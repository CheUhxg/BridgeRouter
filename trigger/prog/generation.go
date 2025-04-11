// Copyright 2015 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package prog

import (
	"math/rand"
)

// Generate generates a random program with ncalls calls.
// ct contains a set of allowed syscalls, if nil all syscalls are used.
func (target *Target) Generate(rs rand.Source, ncalls int, ct *ChoiceTable) *Prog {
	p := &Prog{
		Target: target,
	}
	r := newRand(target, rs)
	s := newState(target, ct, nil)

	// var meta_init *Syscall
	// var meta_final *Syscall

	// for i := range ct.calls {
	// 	if ct.calls[i].ID == 2616 {
	// 		meta_init = ct.calls[i]
	// 	} else if ct.calls[i].ID == 2615 {
	// 		meta_final = ct.calls[i]
	// 	}
	// }
	// if meta_init != nil {
	// 	c_init := MakeCall(meta_init, nil)
	// 	p.Calls = append(p.Calls, c_init)
	// }

	for len(p.Calls) < ncalls {
		calls := r.generateCall(s, p, len(p.Calls))
		for _, c := range calls {
			s.analyze(c)
			p.Calls = append(p.Calls, c)
		}
	}

	// For the last generated call we could get additional calls that create
	// resources and overflow ncalls. Remove some of these calls.
	// The resources in the last call will be replaced with the default values,
	// which is exactly what we want.
	for len(p.Calls) > ncalls {
		p.RemoveCall(ncalls - 1)
	}
	// if meta_final != nil {
	// 	c_final := MakeCall(meta_final, nil)
	// 	p.Calls = append(p.Calls, c_final)
	// }
	p.sanitizeFix()
	p.debugValidate()
	return p
}
